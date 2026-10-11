// camera_pins.h — the DVP pin map for this carrier's camera connector.
// ---------------------------------------------------------------------------
// This is the standard ESP32-S3-CAM DVP wiring, which is what the Freenove
// 2 x 20 carrier uses. Authority: docs/circuit-plan.md section 2 — the whole
// LEFT header row is camera ("it stays 100 % camera"), and those same GPIOs are
// the ones the on-board FPC connector already drives.
//
// !! THE CAMERA IS ALREADY MOUNTED. !! These pins are shared with the ribbon
// connector on the board, so there is NOTHING TO WIRE. This table exists so the
// firmware agrees with the schematic, and so that anyone who later wants one of
// these GPIOs can see exactly what they would be giving up (plan esp32-cam.md:
// dropping the camera is the only way to reclaim 4-18).
//
// Consequence worth remembering: every pin below is a CAMERA pin, so nothing
// else in this project may claim them. The pins that ARE free — the ones this
// firmware uses for audio, I2C and the eventual Pico link — are 21, 41, 42, 43,
// 44, 45, 47, 48. (43/44 carry this project's serial console.)
//
// OV3660, not OV2640 (corrected 2026-10-11, and CONFIRMED on hardware: the driver
// reports PID 0x3660). Both are DVP and both come up with this same pin map and
// the same JPEG pipeline, so the driver needs no change — but the sensor is a
// different part, and it is MOUNTED ROTATED, which is why main.cpp applies a
// vertical flip at boot.
#pragma once

// Not broken out on this board: the carrier ties PWDN and RESET itself.
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1

// Controls
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4      // SCCB data  (the camera's I2C SDA)
#define CAM_PIN_SIOC    5      // SCCB clock (the camera's I2C SCL)

// Parallel data bus, bit 7 down to bit 0.
// These are not in numeric order on the header, and they are not in numeric
// order here either — this matches the module's own silkscreen naming.
#define CAM_PIN_D7      16     // named Y9 on the board
#define CAM_PIN_D6      17     // Y8
#define CAM_PIN_D5      18     // Y7
#define CAM_PIN_D4      12     // Y6
#define CAM_PIN_D3      10     // Y5
#define CAM_PIN_D2      8      // Y4
#define CAM_PIN_D1      9      // Y3
#define CAM_PIN_D0      11     // Y2

// Sync
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

// On-board addressable LED / LED-strip data. NOT a camera pin — it is here
// because this project deliberately uses it as a status/torch light (plan 2.1:
// GPIO48 is the on-board WS2812's data input, and it shares ONE unbuffered net
// with the strip header pin, so the on-board LED mirrors the strip's first
// pixel).
#define PIN_LED_STRIP   48
