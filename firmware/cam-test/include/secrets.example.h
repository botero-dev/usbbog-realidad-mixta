// secrets.example.h — copy to `secrets.h` to use your own credentials.
// ---------------------------------------------------------------------------
// main.cpp includes `secrets.h` when it exists and falls back to THIS file when
// it does not (see the __has_include block there). That means:
//
//   * the project builds and runs with no setup at all, on the throwaway values
//     below, which is what you want for a bench bring-up; and
//   * a real Wi-Fi password never has to live in a file that is copied around.
//
// The defaults below are deliberately obvious rather than secret. An access
// point that anyone in range can join is fine on a bench and is not fine
// anywhere else — change them, or set AP_PASSWORD to "" if you genuinely want
// an open network and understand that anyone can then watch the camera.
#pragma once

#define AP_SSID     "cam-test"
#define AP_PASSWORD "camtest123"   // WPA2 needs >= 8 characters, or "" for open
