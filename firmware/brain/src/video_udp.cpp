// video_udp.cpp — JPEG frames as self-describing UDP datagrams.
// ---------------------------------------------------------------------------
// The unit on the wire is one fragment of one JPEG frame:
//
//   offset  size  field
//   0       2     magic       0x4256 ('V','B' in memory order)
//   2       1     version     1
//   3       1     type        1 = video
//   4       4     frame_id    increments per frame, wraps at 2^32
//   8       4     total_len   size of the whole JPEG
//   12      2     frag_index  0 .. frag_count-1
//   14      2     frag_count  ceil(total_len / 1400)
//   16      2     payload_len bytes of JPEG in this datagram
//   18      2     reserved    zero
//
// All fields are little-endian because that is what the Xtensa writes and what
// the Godot client reads. Every fragment carries the full description, so a
// client never needs a "start" or "end" packet to reassemble: it can start
// listening mid-frame, and a datagram lost at the start costs one frame rather
// than corrupting the parser.
//
// 1400-byte payloads keep the datagram under a 1500-byte MTU (1472 is the
// maximum after IPv4 + UDP headers), so IP never fragments. One lost IP
// fragment would cost the whole datagram anyway; avoiding fragmentation keeps
// the loss accounting honest.
//
// Newest-frame policy: the camera is initialised with CAMERA_GRAB_LATEST, so
// when this task is slower than the camera, esp_camera_fb_get() returns the
// most recent completed frame and the intermediates are discarded by the
// driver. A frame that is already obsolete is never sent.
//
// This task lives on core 0 with the HTTP and audio tasks (see the core map in
// main.cpp). It is independent of both HTTP servers.

#include "video_udp.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_camera.h>
#include <string.h>

namespace video_udp {
namespace {

constexpr uint16_t kPort = 82;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kPayloadBytes = 1400;
constexpr uint16_t kMagic = 0x4256;  // little-endian: bytes 'V', 'B'
constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypeVideo = 1;
constexpr uint32_t kSubscriberTimeoutMs = 2000;
constexpr char kSubscribePacket[] = "BV1 SUB";
constexpr size_t kSubscribeLen = sizeof(kSubscribePacket) - 1;

struct __attribute__((packed)) Header {
  uint16_t magic;
  uint8_t version;
  uint8_t type;
  uint32_t frame_id;
  uint32_t total_len;
  uint16_t frag_index;
  uint16_t frag_count;
  uint16_t payload_len;
  uint16_t reserved;
};
static_assert(sizeof(Header) == kHeaderBytes, "UDP header must stay 20 bytes");

WiFiUDP g_udp;
volatile bool g_active = false;
uint32_t g_frame_id = 0;
IPAddress g_client_ip;
uint16_t g_client_port = 0;
uint32_t g_last_subscribe_ms = 0;
uint32_t g_frames_sent = 0;
uint32_t g_fragments_sent = 0;

void note_subscriber(uint32_t now_ms) {
  const IPAddress ip = g_udp.remoteIP();
  const uint16_t port = g_udp.remotePort();
  if (port == 0) return;

  if (!g_active || ip != g_client_ip || port != g_client_port) {
    Serial.printf("[udp] subscriber %s:%u\n", ip.toString().c_str(),
                  (unsigned)port);
  }
  g_client_ip = ip;
  g_client_port = port;
  g_last_subscribe_ms = now_ms;
  g_active = true;
}

// Reads every pending datagram. Only subscribed clients are honoured, and only
// if the payload looks like our subscribe packet: an open UDP port otherwise
// streams a camera into whatever happens to probe it.
void poll_subscriptions(uint32_t now_ms) {
  while (g_udp.parsePacket() > 0) {
    uint8_t first[8] = {0};
    const int n = g_udp.read(first, sizeof(first));
    while (g_udp.available() > 0) {
      uint8_t junk[32];
      g_udp.read(junk, sizeof(junk));
    }
    if (n >= (int)kSubscribeLen &&
        memcmp(first, kSubscribePacket, kSubscribeLen) == 0) {
      note_subscriber(now_ms);
    }
  }
}

// Sends one JPEG as ceil(len/payload) self-describing datagrams. Stops early if
// the client disappears mid-frame, so the next frame is not delayed by a
// stream nobody is reading.
void send_frame(const camera_fb_t *fb) {
  const uint16_t count =
      (uint16_t)((fb->len + kPayloadBytes - 1) / kPayloadBytes);
  if (count == 0) return;

  const uint32_t frame_id = ++g_frame_id;
  uint16_t sent = 0;

  for (uint16_t i = 0; i < count; ++i) {
    const size_t offset = (size_t)i * kPayloadBytes;
    const size_t len = min(kPayloadBytes, fb->len - offset);

    Header header = {};
    header.magic = kMagic;
    header.version = kVersion;
    header.type = kTypeVideo;
    header.frame_id = frame_id;
    header.total_len = (uint32_t)fb->len;
    header.frag_index = i;
    header.frag_count = count;
    header.payload_len = (uint16_t)len;

    if (!g_udp.beginPacket(g_client_ip, g_client_port)) break;
    const size_t wrote_header =
        g_udp.write(reinterpret_cast<const uint8_t *>(&header), kHeaderBytes);
  const size_t wrote_payload = g_udp.write(fb->buf + offset, len);
  const bool complete =
      wrote_header == kHeaderBytes && wrote_payload == len;
  // endPacket() must always follow beginPacket(): it frees the transmit
  // buffer. If the writes did not fit, the datagram is malformed and the
  // client rejects it (payload_len > datagram size), so abandon the frame.
  const bool packet_sent = g_udp.endPacket();
  if (!complete || !packet_sent) break;
    sent++;

    // Poll every 8 fragments: cheap, and it notices a vanishing client within
    // a frame instead of after the full 2 s timeout.
    if ((i & 7) == 7) {
      const uint32_t now = millis();
      poll_subscriptions(now);
      if (!g_active) break;
    }
  }

  g_frames_sent++;
  g_fragments_sent += sent;
}

void task(void *) {
  if (!g_udp.begin(kPort)) {
    Serial.printf("[udp] FAILED to bind UDP port %u\n", (unsigned)kPort);
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("[udp] video on :%u/udp, newest frame only\n", (unsigned)kPort);

  uint32_t fps_mark = millis();
  uint32_t fps_frames = 0;

  while (true) {
    const uint32_t now = millis();
    poll_subscriptions(now);

    if (g_active && (uint32_t)(now - g_last_subscribe_ms) > kSubscriberTimeoutMs) {
      Serial.printf("[udp] subscriber %s:%u timed out\n",
                    g_client_ip.toString().c_str(), (unsigned)g_client_port);
      g_active = false;
    }

    if (!g_active) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // CAMERA_GRAB_LATEST is what makes this "newest frame": when the network is
    // slower than the camera, the driver overwrites buffers we have not seen and
    // this returns the current one. It may block for at most one frame.
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (fb->format == PIXFORMAT_JPEG) {
      send_frame(fb);
      fps_frames++;
    }
    esp_camera_fb_return(fb);

    const uint32_t after = millis();
    if (after - fps_mark >= 1000) {
      const uint32_t fps = fps_frames * 1000u / (after - fps_mark);
      Serial.printf("[udp] %u fps  frames %u  frags %u  -> %s:%u\n",
                    (unsigned)fps, (unsigned)g_frames_sent,
                    (unsigned)g_fragments_sent, g_client_ip.toString().c_str(),
                    (unsigned)g_client_port);
      fps_mark = after;
      fps_frames = 0;
    }
  }
}

}  // namespace

bool start() {
  const BaseType_t ok = xTaskCreatePinnedToCore(task, "video_udp", 6144, nullptr,
                                                4, nullptr, 0);
  if (ok != pdPASS) return false;
  Serial.println("[udp] sender task started; waiting for a subscriber");
  return true;
}

}  // namespace video_udp
