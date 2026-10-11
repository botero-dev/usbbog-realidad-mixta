// secrets.example.h — copy to `secrets.h` to use your own credentials.
// ---------------------------------------------------------------------------
// main.cpp includes `secrets.h` when it exists and falls back to THIS file when
// it does not (see the __has_include block there). That means:
//
//   * the project builds and runs with no setup at all, on the throwaway values
//     below, which is what you want for a bench bring-up; and
//   * a real Wi-Fi password never has to live in a file that is copied around.
//
// The SSID is `brain`, not `cam-test`, on purpose: it tells you which project is
// flashed without opening a serial console. An access point that anyone in range
// can join is fine on a bench and is not fine anywhere else — change these, or
// set AP_PASSWORD to "" if you genuinely want an open network and understand that
// anyone can then watch the camera.
#pragma once

#define AP_SSID     "brain"
#define AP_PASSWORD "brainpass"  // WPA2 needs >= 8 characters, or "" for open
