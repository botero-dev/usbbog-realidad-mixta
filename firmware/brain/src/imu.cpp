// imu.cpp — BMI160 over I2C. See imu.h for the shape of the module.
// ---------------------------------------------------------------------------
// Pins: SDA = GPIO41, SCL = GPIO42, address 0x69.
// Authority: docs/circuit-plan.md section 5.
//
// *** FOUR DECISIONS IN HERE THAT LOOK ARBITRARY AND ARE NOT. ***
//
// 1. THE I2C CONTROLLER IS DISCOVERED, NOT ASSUMED.
//    The camera's SCCB bus takes one of the two controllers, and which one is an
//    esp32-camera internal that this project does not control. The first version of
//    this file assumed I2C_NUM_1 was free because the camera was believed to use
//    I2C_NUM_0. It was not, and the boot log said so:
//        E (431) i2c: i2c driver install error
//        [imu] i2c_driver_install failed (is I2C_NUM_1 in use?)
//    So the bus is now installed on whichever controller ACCEPTS it, trying
//    I2C_NUM_0 first because that is the one the measurement left free. Do not
//    "tidy" this into a single constant: the right constant is not knowable from
//    here, and getting it wrong presents as a dead sensor rather than a busy bus.
//
// 2. NO EXTERNAL PULL-UPS, and the internal ones are DISABLED here.
//    The module carries its own 3.9k to its own 3V3 rail (measured, plan section 5),
//    so fitting more would only load the bus harder. The ESP32's internal pull-ups
//    are ~45k -- far too weak to matter at 400 kHz -- so leaving them off is both
//    harmless and honest: the pull-ups that make this bus work are on the module.
//
// 3. The address is 0x69, NOT 0x68. SA0 is pulled up on this module, so a driver
//    hunting 0x68 finds nothing and looks like dead hardware. This is the single
//    most valuable line in section 5.
//
// 4. THE BUS RUNS AT 100 kHz, NOT THE 400 kHz THAT PLAN SECTION 5.2 ASKS FOR.
//    Measured 2026-10-11: at 400 kHz this part ACKs and READS fine, but CONFIG WRITES
//    ARE DROPPED -- the accelerometer comes up at 0.000 g while PMU_STATUS insists it
//    is in normal mode. The project had always run at 100 kHz BY ACCIDENT: a wrong
//    CHIP_ID constant made identification fall through to the 100 kHz fallback on every
//    boot. Fixing that bug is what revealed this. See the note on kBusHz.
// ---------------------------------------------------------------------------

#include "imu.h"

#include <Arduino.h>
#include <driver/i2c.h>
#include <math.h>

namespace {

constexpr gpio_num_t kSda = GPIO_NUM_41;
constexpr gpio_num_t kScl = GPIO_NUM_42;

// *** 100 kHz, AND THAT IS A MEASUREMENT, NOT A TIMID DEFAULT. ***
// Plan section 5.2 funds 400 kHz and the module's own pull-ups are rated for it, but
// the WIRING is not. Measured 2026-10-11: at 400 kHz the part ACKs and reads come back
// stable, while CONFIG WRITES DO NOT LAND. ACC_CONF read back 0x28 and ACC_RANGE 0x01
// -- neither of them a value that was written -- and the accelerometer then reported
// 0.000 g on all three axes while PMU_STATUS still claimed "accel normal". At 100 kHz
// the same writes take and the gravity check passes at ~1.05 g.
//
// This was invisible for the life of the project: the wrong CHIP_ID constant made
// identify_chip() fall through to its 100 kHz fallback on every single boot, so 100 kHz
// is what the firmware had ALWAYS used -- by accident. Fixing that bug is what exposed
// this. Writing the number down explicitly is what turns an accident into a decision.
//
// WANT 400 kHz BACK? The experiment is SETTLING TIME, not pull-ups: insert a few ms
// after each config write and retest. Reads survive 400 kHz, so signal integrity alone
// does not explain it. The leading theory is that the part silently drops a write which
// arrives before it has digested the previous one, and at 100 kHz the slower
// transactions were supplying that gap for free.
constexpr uint32_t kBusHz = 100000;

constexpr uint8_t kAddr = 0x69;  // SA0 pulled up on the module
constexpr uint8_t kAddrAlt = 0x68;

// Set by install_bus(); the only things that know what we actually got.
i2c_port_t s_port = I2C_NUM_0;
uint32_t s_bus_hz = kBusHz;
gpio_num_t s_sda = kSda;
gpio_num_t s_scl = kScl;
bool s_swapped = false;  // running with SDA/SCL the other way round vs the plan

constexpr uint8_t REG_CHIP_ID = 0x00;
constexpr uint8_t REG_PMU_STATUS = 0x03;
constexpr uint8_t REG_DATA = 0x0C;  // 12 bytes: gyro xyz then accel xyz
constexpr uint8_t REG_TEMP = 0x20;  // 2 bytes, read at 1 Hz only
constexpr uint8_t REG_ACC_CONF = 0x40;
constexpr uint8_t REG_ACC_RANGE = 0x41;
constexpr uint8_t REG_GYR_CONF = 0x42;
constexpr uint8_t REG_GYR_RANGE = 0x43;
constexpr uint8_t REG_CMD = 0x7E;

// *** 0xD1 IS THE BMI160. THIS FILE USED TO SAY 0x24, AND THAT WAS WRONG. ***
// Corrected 2026-10-11 against Bosch's own driver (BMI160_driver v3.9.2,
// bmi160_defs.h), which states it outright:
//     /** BMI160 unique chip identifier */
//     #define BMI160_CHIP_ID      UINT8_C(0xD1)
// 0x24 is the BMI270 -- the newer part that needs an ~8 KB config blob uploaded at
// every boot before it produces a single sample.
//
// The old value cost more than a wrong comment, and it was invisible:
// identify_chip() returns early only when it reads THIS constant, so with 0x24 here
// nothing ever matched. It ran all four orientation/speed combinations, then fell
// through to reinstall(kSda, kScl, 100000) -- which left the bus at the 100 kHz
// FALLBACK for the entire session, at 200 Hz, forever. That is ~1.3 ms per read
// instead of ~330 us: ~26% of the time rather than ~7%. The boot log said
// "bus 100000 Hz" throughout and looked perfectly ordinary.
//
// BMI160 and BMX160 share this ID, so it cannot distinguish them -- which matters
// only for the magnetometer question, and is why there is one constant, not two.
constexpr uint8_t CHIP_ID_BMI160 = 0xD1;
constexpr uint8_t CHIP_ID_BMI270 = 0x24;  // recognised, but not usable by this driver

// Sensitivity, DERIVED FROM THE RANGE REGISTERS at boot -- not assumed from the
// values we tried to write. This board is the reason why. The accel range write did
// not take, the chip kept its +-2 g default, and a stationary board read 4.2 g:
// that is 1.05 g once divided by 4, i.e. real data at the wrong scale. A scaling bug
// like that looks completely plausible on a graph, which is exactly what makes it
// worth removing by construction. Read the range back; derive the divisor.
float s_acc_lsb_per_g = 4096.0f;
float s_gyr_lsb_per_dps = 16.4f;

// *** WHY THIS FUDGE EXISTS, AND WHY IT IS NOT HIDDEN IN A NUMBER. ***
//
// ACC_RANGE reads back 0x02, holds 0x02, and the chip still behaves like +-2 g.
// Measured on 2026-10-11: a stationary board returned raw |a| ~= 17200 counts, which
// is 4.20 g at the datasheet's 4096 LSB/g for that range, and 1.05 g at 16384.
// Gravity is 1 g, so the part is reporting at +-2 g whatever the register says.
//
// This is consistent with the rest of what this chip does: GYR_CONF silently keeps
// its reset ODR, so two registers disagree with this part's own datasheet. The ID is
// NOT one of them -- 0xD1 is the ordinary BMI160 value. The rule that follows is the
// useful one: TRUST THE PHYSICS OVER THE
// REGISTER. A stationary board MUST read 1 g, and report_gravity() says so out loud
// one second after boot, so the next board cannot hide the same mistake.
//
// 1.0 means "the range register tells the truth". 4.0 means the +-2 g data is being
// reported under a +-8 g register setting.
constexpr float kAccelRangeCorrection = 4.0f;

// Accumulators for the one-second gravity check. Written and read only by the
// sampler task, so they need no lock.
float s_acc_mag_sum = 0.0f;
uint32_t s_acc_mag_count = 0;

float acc_lsb_per_g(uint8_t range_reg) {
  // ACC_RANGE: 0 = +-2 g, 1 = +-4 g, 2 = +-8 g, 3 = +-16 g
  switch (range_reg & 0x03) {
    case 0: return 16384.0f;
    case 1: return 8192.0f;
    case 3: return 2048.0f;
    default: return 4096.0f;
  }
}

float gyr_lsb_per_dps(uint8_t range_reg) {
  // GYR_RANGE: 0 = +-2000 dps, 1 = +-1000, 2 = +-500, 3 = +-250, 4 = +-125
  switch (range_reg & 0x07) {
    case 1: return 32.8f;
    case 2: return 65.6f;
    case 3: return 131.2f;
    case 4: return 262.4f;
    default: return 16.4f;
  }
}

constexpr uint32_t kTaskHz = 200;
constexpr uint32_t kTaskPeriodMs = 1000 / kTaskHz;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
ImuSample s_sample = {};
bool s_present = false;
uint8_t s_who = 0;

// --- transactions ---------------------------------------------------------
esp_err_t write_reg(uint8_t reg, uint8_t value) {
  const uint8_t buf[2] = {reg, value};
  return i2c_master_write_to_device(s_port, kAddr, buf, sizeof(buf),
                                    pdMS_TO_TICKS(50));
}

esp_err_t read_regs(uint8_t reg, uint8_t *dst, size_t n) {
  return i2c_master_write_read_device(s_port, kAddr, &reg, 1, dst, n,
                                      pdMS_TO_TICKS(50));
}

// A bare address probe: start, address+write, stop. An ACK means something is there.
esp_err_t probe(uint8_t addr) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
  i2c_master_stop(cmd);
  const esp_err_t res = i2c_master_cmd_begin(s_port, cmd, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(cmd);
  return res;
}

// --- configuration --------------------------------------------------------

// Defined further down, with the bus code: identification has to run before
// anything is WRITTEN, and it reads better as a unit next to the bus setup.
uint8_t identify_chip();
bool reinstall(gpio_num_t sda, gpio_num_t scl, uint32_t hz);

bool configure() {
  // *** IDENTIFY FIRST, WRITE SECOND. ***
  // This function used to open with a soft reset -- which writes 0xB6 to register
  // 0x7E. On a part that is NOT a BMI160, 0x7E means something else entirely, so
  // writing to it before identifying the chip is how you corrupt state and then
  // blame the driver. Read-only identification costs nothing and always comes first.
  // identify_chip() prints the orientation/speed table; its RETURN value is a
  // sentinel (0 = nothing identified), not a register read. An earlier version of
  // this log printed it as "0x00(CHIP_ID)", which claimed a measurement that was
  // never made.
  identify_chip();

  uint8_t id00 = 0, id0f = 0, id75 = 0, err_reg = 0;
  if (read_regs(REG_CHIP_ID, &id00, 1) != ESP_OK) id00 = 0xFF;
  if (read_regs(0x0F, &id0f, 1) != ESP_OK) id0f = 0;
  if (read_regs(0x75, &id75, 1) != ESP_OK) id75 = 0;
  if (read_regs(0x02, &err_reg, 1) != ESP_OK) err_reg = 0;
  s_who = id00;

  Serial.printf("[imu] identity: 0x00=0x%02x  0x0F=0x%02x  0x75=0x%02x  "
                "0x02(ERR)=0x%02x   (bus %u Hz)\n",
                id00, id0f, id75, err_reg, (unsigned)s_bus_hz);
  Serial.println("[imu]   legend: 0x00 0xD1=BMI160/BMX160 0x24=BMI270 | 0x0F "
                 "0x69/0x6A/0x6C/0x6B = ST LSM6DSx | 0x75 0x68/0x70/0x71 = InvenSense");

  // 0xD1 is the BMI160's own ID (see the note on the constant) and the only ID this
  // driver targets. Measured on this board: 0xD1, stable across repeated reads AND
  // at both 400 kHz and 100 kHz, in the plan's pin orientation -- so it is a real
  // report from a real part, and the EXPECTED one. It is not an anomaly, and it is
  // not evidence of anything.
  //
  // Any other ID means a different part with a different register map, so stop here
  // rather than write to registers that mean something else on that part.
  if (id00 != CHIP_ID_BMI160) {
    Serial.printf("[imu] CHIP_ID 0x%02x is not 0xD1 (BMI160/BMX160).\n", id00);
    if (id00 == CHIP_ID_BMI270) {
      Serial.println("[imu] 0x24 is a BMI270: the same family, but it needs an ~8 KB");
      Serial.println("[imu] config blob uploaded at EVERY boot before it outputs data.");
      Serial.println("[imu] This driver does not do that -- see esp32-cam.md section 13.");
    } else if (id0f == 0x69 || id0f == 0x6A || id0f == 0x6C || id0f == 0x6B) {
      Serial.printf("[imu] 0x0F = 0x%02x is an ST LSM6DSx WHO_AM_I: a different part\n",
                    id0f);
      Serial.println("[imu] with a different register map.");
    } else if (id75 == 0x68 || id75 == 0x70 || id75 == 0x71 || id75 == 0x73) {
      Serial.printf("[imu] 0x75 = 0x%02x is an InvenSense MPU-family WHO_AM_I\n", id75);
      Serial.println("[imu] (0x68 MPU-6050, 0x70 MPU-6500, 0x71 MPU-9250). It shares");
      Serial.println("[imu] address 0x68 with the BMI160, which is why the two get");
      Serial.println("[imu] mistaken for each other -- and it needs a different driver.");
    } else if (id00 == 0x00 || id00 == 0xFF) {
      Serial.println("[imu] all-zeroes or all-ones: the bus answered but nothing drove");
      Serial.println("[imu] it. Suspect CS held LOW (that selects SPI mode) or a bad");
      Serial.println("[imu] SDA/SCL joint.");
    } else {
      Serial.println("[imu] Unrecognised part. Read the module's silkscreen.");
    }
    return false;
  }

  // *** THE ID CANNOT SETTLE THE MAGNETOMETER QUESTION. ***
  // BMI160 and BMX160 both report 0xD1; they differ only in whether a magnetometer
  // shares the package. So the READINGS are what test the assumption, not the ID: a
  // still board must read ~1 g on one accel axis and ~0 dps on all three, and
  // report_gravity() states that outright one second after boot.

  // Soft reset, now that we know what we are talking to. A warm reset (EN pin) does
  // NOT reset the BMI160 -- it keeps whatever the last firmware left in it -- so
  // skipping this would make behaviour depend on history.
  write_reg(REG_CMD, 0xB6);
  delay(100);

  // *** CONFIGURE WHILE SUSPENDED. ***
  // The BMI160 only accepts ODR and range changes while the sensor is in SUSPEND
  // mode; writes made in normal mode are silently dropped. This code used to trust
  // the soft reset to have left it suspended, and the accel range write did not
  // take -- which surfaced as a stationary board reading 4.2 g.
  write_reg(REG_CMD, 0x10);  // accel -> suspend
  delay(10);
  write_reg(REG_CMD, 0x14);  // gyro  -> suspend
  delay(10);

  // Accel: 100 Hz, normal bandwidth, +-8 g   (plan section 5.1)
  write_reg(REG_ACC_CONF, (uint8_t)((0x08 << 4) | 0x0A));
  write_reg(REG_ACC_RANGE, 0x02);  // 0x02 = +-8 g

  // Gyro: 200 Hz, normal bandwidth, +-2000 dps
  write_reg(REG_GYR_CONF, (uint8_t)((0x09 << 4) | 0x0A));
  write_reg(REG_GYR_RANGE, 0x00);  // 0x00 = +-2000 dps

  // Read the configuration BACK, before enabling anything, so the log shows what
  // the chip accepted rather than what we hoped for. Then derive the scale factors
  // from those registers -- see the note on s_acc_lsb_per_g.
  uint8_t ac = 0, ar = 0, gc = 0, gr = 0;
  read_regs(REG_ACC_CONF, &ac, 1);
  read_regs(REG_ACC_RANGE, &ar, 1);
  read_regs(REG_GYR_CONF, &gc, 1);
  read_regs(REG_GYR_RANGE, &gr, 1);
  Serial.printf("[imu] config readback: ACC_CONF 0x%02x RANGE 0x%02x | GYR_CONF "
                "0x%02x RANGE 0x%02x\n", ac, ar, gc, gr);
  Serial.println("[imu]             wanted: ACC_CONF 0x8a RANGE 0x02 | GYR_CONF "
                 "0x9a RANGE 0x00");

  s_acc_lsb_per_g = acc_lsb_per_g(ar) * kAccelRangeCorrection;
  s_gyr_lsb_per_dps = gyr_lsb_per_dps(gr);
  Serial.printf("[imu] using: accel %.0f LSB/g (register %.0f x %.1f correction), "
                "gyro %.1f LSB/dps\n",
                s_acc_lsb_per_g, acc_lsb_per_g(ar), kAccelRangeCorrection,
                s_gyr_lsb_per_dps);
  if (gc != (uint8_t)((0x09 << 4) | 0x0A)) {
    Serial.printf("[imu] NOTE: GYR_CONF reads 0x%02x, not 0x9a -- the ODR field did "
                  "not take.\n", gc);
    Serial.println("[imu] The gyro still reads, but slower than 200 Hz, so consecutive");
    Serial.println("[imu] samples repeat. Fine for tilt; check it before integrating.");
  }

  // Enter normal mode. The command register takes one value at a time, so this is
  // two writes, and each takes a few ms to complete.
  if (write_reg(REG_CMD, 0x11) != ESP_OK) return false;  // accel -> normal
  delay(10);
  if (write_reg(REG_CMD, 0x15) != ESP_OK) return false;  // gyro  -> normal
  delay(100);

  // Read back the power state: 0x14 means both gyro (bits 7:6) and accel (bits 5:4)
  // are in normal mode. Cheap, and it turns "the readings never change" into a
  // one-line diagnosis.
  uint8_t pmu = 0;
  if (read_regs(REG_PMU_STATUS, &pmu, 1) == ESP_OK) {
    Serial.printf("[imu] PMU status 0x%02x (0x14 = both normal)\n", pmu);
  }
  return true;
}

// --- sampling -------------------------------------------------------------
void read_once() {
  uint8_t raw[12];
  if (read_regs(REG_DATA, raw, sizeof(raw)) != ESP_OK) {
    s_sample.errors++;
    return;
  }

  const int16_t gx = (int16_t)((raw[1] << 8) | raw[0]);
  const int16_t gy = (int16_t)((raw[3] << 8) | raw[2]);
  const int16_t gz = (int16_t)((raw[5] << 8) | raw[4]);
  const int16_t ax = (int16_t)((raw[7] << 8) | raw[6]);
  const int16_t ay = (int16_t)((raw[9] << 8) | raw[8]);
  const int16_t az = (int16_t)((raw[11] << 8) | raw[10]);

  const float fx = ax / s_acc_lsb_per_g;
  const float fy = ay / s_acc_lsb_per_g;
  const float fz = az / s_acc_lsb_per_g;

  // Fed into the one-second gravity check. Free here, and it is the only thing that
  // can tell a correct scale from a plausible-looking wrong one.
  s_acc_mag_sum += sqrtf(fx * fx + fy * fy + fz * fz);
  s_acc_mag_count++;

  // Tilt from the accelerometer: measures which way gravity points, so it is
  // absolute and does not drift -- unlike a gyro integral. It is also noisy while
  // the car is moving, which is why plan section 5.3 says the wheels and the camera
  // supply the heading reference rather than this.
  const float pitch = atan2f(-fx, sqrtf(fy * fy + fz * fz)) * 180.0f / (float)M_PI;
  const float roll = atan2f(fy, fz) * 180.0f / (float)M_PI;

  taskENTER_CRITICAL(&s_mux);
  s_sample.ax = fx;
  s_sample.ay = fy;
  s_sample.az = fz;
  s_sample.gx = gx / s_gyr_lsb_per_dps;
  s_sample.gy = gy / s_gyr_lsb_per_dps;
  s_sample.gz = gz / s_gyr_lsb_per_dps;
  s_sample.pitch = pitch;
  s_sample.roll = roll;
  s_sample.seq++;
  taskEXIT_CRITICAL(&s_mux);
}

void read_temperature() {
  uint8_t t[2];
  if (read_regs(REG_TEMP, t, sizeof(t)) != ESP_OK) return;
  const int16_t raw = (int16_t)((t[1] << 8) | t[0]);
  // BMI160: 1/512 degC per LSB, 23 degC offset. A sanity check, not a thermometer
  // worth trusting to a degree.
  const float c = 23.0f + (float)raw / 512.0f;
  taskENTER_CRITICAL(&s_mux);
  s_sample.temp_c = c;
  taskEXIT_CRITICAL(&s_mux);
}

// *** THE CHECK THAT WOULD HAVE CAUGHT THE SCALE BUG IMMEDIATELY. ***
//
// A stationary board MUST read 1 g. Anything else is a range or scaling error, and
// it is worth saying so on every boot, because the failure mode is a graph that looks
// perfectly plausible with the wrong numbers on it. Nobody notices until the control
// loop misbehaves, and by then the cause is three layers away.
void report_gravity() {
  if (s_acc_mag_count == 0) return;
  const float mag = s_acc_mag_sum / (float)s_acc_mag_count;
  Serial.printf("[imu] resting |a| = %.3f g over %u samples (still board = 1.000)\n",
                mag, (unsigned)s_acc_mag_count);
  if (mag < 0.8f || mag > 1.25f) {
    Serial.printf("[imu] *** WRONG BY A FACTOR OF %.2f ***\n", mag);
    Serial.println("[imu] Either the board was moving during the first second, or the");
    Serial.println("[imu] scale does not match the part. Check ACC_RANGE against the raw");
    Serial.println("[imu] counts and set kAccelRangeCorrection accordingly.");
  } else {
    Serial.println("[imu] gravity check PASSED -- the accelerometer scale is right");
  }
  s_acc_mag_sum = 0.0f;
  s_acc_mag_count = 0;
}

void imu_task(void *) {
  TickType_t last = xTaskGetTickCount();
  uint32_t tick = 0;
  for (;;) {
    read_once();
    if (++tick % kTaskHz == 0) {
      read_temperature();                        // ~1 Hz
      if (tick == kTaskHz) report_gravity();     // once, one second in
    }
    // Fixed rate, never delay() -- plan section 10.1. vTaskDelayUntil keeps the
    // period honest even when a read runs long, instead of drifting.
    vTaskDelayUntil(&last, pdMS_TO_TICKS(kTaskPeriodMs));
  }
}

bool try_install(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, uint32_t hz) {
  i2c_config_t cfg = {};
  cfg.mode = I2C_MODE_MASTER;
  cfg.sda_io_num = sda;
  cfg.scl_io_num = scl;
  // Disabled: the module's own 3.9k pull-ups do this job. See the note at the top.
  cfg.sda_pullup_en = GPIO_PULLUP_DISABLE;
  cfg.scl_pullup_en = GPIO_PULLUP_DISABLE;
  cfg.master.clk_speed = hz;

  if (i2c_param_config(port, &cfg) != ESP_OK) return false;
  if (i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) return false;

  s_port = port;
  s_sda = sda;
  s_scl = scl;
  s_bus_hz = hz;
  return true;
}

// Install our driver on the first controller that will have us. A driver-install
// failure is exactly the signal that the port is taken, so trying the other one is
// safe and needs no knowledge of what the camera did.
bool install_bus() {
  for (const i2c_port_t port : {I2C_NUM_0, I2C_NUM_1}) {
    if (try_install(port, kSda, kScl, kBusHz)) return true;
  }
  return false;
}

// Reinstall the SAME controller with, possibly, different PINS and a different
// speed. Both have to be settable: a swapped-cable test that kept the pins would
// test nothing at all.
bool reinstall(gpio_num_t sda, gpio_num_t scl, uint32_t hz) {
  i2c_driver_delete(s_port);
  return try_install(s_port, sda, scl, hz);
}

// *** THE DIAGNOSTIC THAT ANSWERS THREE QUESTIONS AT ONCE. ***
//
// One line per orientation/speed, so the boot log reads as a table and the answer
// is visible rather than argued about:
//
//   is this really a BMI160?      -> does the ID read 0xD1
//   ARE SDA AND SCL SWAPPED?      -> does the OTHER orientation read 0xD1
//   is the bus corrupting data?   -> does the SAME read give different values
//
// The middle question is why this exists. Both I2C lines idle high, so a swapped
// pair still gets the address ACKed -- the chip really does answer -- while the data
// bits come back as nonsense. "Something is there, but its ID is wrong" is EXACTLY
// what a swapped pair looks like, and it is the cheapest wiring mistake to make and
// the hardest to see.
uint8_t try_combo(const char *label, gpio_num_t sda, gpio_num_t scl, uint32_t hz) {
  if (!reinstall(sda, scl, hz)) {
    Serial.printf("[imu] %-26s bus install FAILED\n", label);
    return 0;
  }

  Serial.printf("[imu] %-26s probe 0x69: %-5s ID:", label,
                (probe(kAddr) == ESP_OK) ? "ACK" : "noACK");

  uint8_t last = 0;
  bool varied = false;
  for (int i = 0; i < 8; ++i) {
    uint8_t v = 0;
    if (read_regs(REG_CHIP_ID, &v, 1) == ESP_OK) {
      Serial.printf(" %02x", v);
      if (i > 0 && v != last) varied = true;
      last = v;
    } else {
      Serial.print(" --");
      varied = true;
    }
  }
  Serial.printf("  %s\n", varied ? "VARIES" : "stable");
  return last;
}

uint8_t identify_chip() {
  // The plan orientation at the CONFIGURED speed comes first, and the order is the
  // point: 100 kHz is the speed whose writes land (see the note on kBusHz), so
  // accepting the part at 400 kHz would hand the driver a bus it cannot configure.
  uint8_t id = try_combo("SDA41/SCL42 @100k plan", kSda, kScl, kBusHz);
  if (id == CHIP_ID_BMI160) return id;

  id = try_combo("SDA42/SCL41 @100k SWAPPED", kScl, kSda, kBusHz);
  if (id == CHIP_ID_BMI160) {
    s_swapped = true;
    Serial.println("[imu] *** SDA AND SCL ARE SWAPPED. ***");
    Serial.println("[imu] Plan section 2.4 puts SCL on GPIO42 and SDA on GPIO41. The chip");
    Serial.println("[imu] only answers with them the other way round, so the WIRING is swapped");
    Serial.println("[imu] and this firmware is using the swapped pair. Swap the two jumpers and");
    Serial.println("[imu] delete this fallback, or the next board built to the plan is wrong.");
    return id;
  }

  // Nothing at the working speed, in either orientation. That makes this a real
  // "no part / bad joint" report rather than a speed problem.
  //
  // 400 kHz is still tried, but ONLY as a read-only second opinion: it separates
  // "nothing is there at all" from "something is there that this wiring cannot
  // configure", which is precisely the state that produced 0.000 g on 2026-10-11.
  // Whatever it says, the bus goes back to the speed that works -- a part that answers
  // only at 400 kHz is not usable by this driver.
  try_combo("SDA41/SCL42 @400k plan", kSda, kScl, 400000);
  try_combo("SDA42/SCL41 @400k SWAPPED", kScl, kSda, 400000);

  // Leave the bus in the PLAN orientation at the working speed, so the diagnostic
  // reads that follow describe the wiring the schematic asks for rather than the last
  // thing we happened to try.
  reinstall(kSda, kScl, kBusHz);
  return 0;
}

}  // namespace

int imu_scan(char *out, size_t out_len) {
  int found = 0;
  size_t used = 0;
  if (out_len > 0) out[0] = '\0';

  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    if (probe(addr) != ESP_OK) continue;
    found++;
    const int n = snprintf(out + used, out_len - used, "0x%02x ", addr);
    if (n > 0 && (size_t)n < out_len - used) {
      used += (size_t)n;
    } else {
      break;  // buffer full: report the count, not a truncated list
    }
  }
  return found;
}

bool imu_init() {
  if (!install_bus()) {
    // Both controllers refused, which means something else has installed both --
    // not a wiring problem, and not something the sensor can fix.
    Serial.println("[imu] could not get an I2C controller: BOTH are already in use");
    Serial.println("[imu] (the camera's SCCB bus takes one; something else has the other)");
    return false;
  }
  Serial.printf("[imu] I2C bus up: controller %d, SDA%d/SCL%d, %u Hz\n", (int)s_port,
                (int)s_sda, (int)s_scl, (unsigned)s_bus_hz);

  char list[96];
  const int n = imu_scan(list, sizeof(list));
  if (n == 0) {
    Serial.printf("[imu] scan on SDA%d/SCL%d: NOTHING responded\n", (int)s_sda,
                  (int)s_scl);
    Serial.println("[imu] check, in this order:");
    Serial.println("[imu]   1. VIN -> +5V (NOT 3V3) and GND -> GND");
    Serial.println("[imu]   2. SCL -> GPIO42, SDA -> GPIO41");
    Serial.println("[imu]   3. CS and SA0 both LEFT OPEN (the module pulls them high:");
    Serial.println("[imu]      CS high = I2C mode, SA0 high = address 0x69)");
    return false;
  }
  Serial.printf("[imu] scan on SDA%d/SCL%d: %d device(s): %s\n", (int)s_sda, (int)s_scl,
                n, list);

  if (probe(kAddr) != ESP_OK) {
    Serial.printf("[imu] no device at 0x%02x.\n", kAddr);
    if (probe(kAddrAlt) == ESP_OK) {
      Serial.println("[imu] but SOMETHING is at 0x68 -- so SA0 is pulled LOW, not");
      Serial.println("[imu] high, and the address in this firmware is wrong.");
    }
    return false;
  }

  if (!configure()) {
    // configure() has already printed what answered and what it looked like.
    Serial.println("[imu] not starting the sampler.");
    Serial.println("[imu] note: a BMI270 answers 0x00 = 0x24 as well, but needs an");
    Serial.println("[imu] ~8 KB config blob uploaded at every boot. This driver does not");
    Serial.println("[imu] do that -- see docs/esp32-cam.md section 13.");
    return false;
  }

  s_present = true;
  Serial.printf("[imu] BMI160-family ready: CHIP_ID 0x%02x, sampling at %u Hz on "
                "core 1, bus %u Hz\n",
                s_who, (unsigned)kTaskHz, (unsigned)s_bus_hz);
  if (s_swapped) {
    Serial.println("[imu] WARNING: running with SDA/SCL swapped relative to the plan.");
  }
  // Priority 5 on core 1 is the control task's slot from plan section 10.1. The
  // sampler goes there now so the control law can join it later without moving.
  xTaskCreatePinnedToCore(imu_task, "imu", 4096, nullptr, 5, nullptr, 1);
  return true;
}

bool imu_present() { return s_present; }

uint8_t imu_who_am_i() { return s_who; }

uint32_t imu_rate_hz() { return kTaskHz; }

void imu_latest(ImuSample *out) {
  if (!out) return;
  taskENTER_CRITICAL(&s_mux);
  *out = s_sample;
  taskEXIT_CRITICAL(&s_mux);
}
