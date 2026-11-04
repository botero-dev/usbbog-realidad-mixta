// video_udp.cpp — the car's UDP link: JPEG frames and IMU samples.
// ---------------------------------------------------------------------------
// The unit on the wire is one datagram with a 20-byte header. `type` says what
// the payload is, and both types share one socket, one port and one subscriber:
//
//   offset  size  field
//   0       2     magic       0x4256 ('V','B' in memory order)
//   2       1     version     1
//   3       1     type        1 = video fragment, 2 = IMU sample
//   4       4     frame_id    per type: frame counter / sampler `seq`
//   8       4     total_len   size of the whole payload
//   12      2     frag_index  0 .. frag_count-1
//   14      2     frag_count  ceil(total_len / 1400); 1 for IMU
//   16      2     payload_len bytes in this datagram
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
// An IMU sample (type 2) is always ONE datagram of 20 + 44 bytes, sent at
// kImuHz -- not at the sampler's 200 Hz, which would buy airtime and nothing a
// person can read. Its policy is the video's: the newest sample is sent, and a
// sample that misses its slot is dropped rather than queued, so a client can
// never be shown a backlog of stale attitudes.
//
// One socket, two tasks. WiFiUDP owns a single transmit buffer and is not
// reentrant, so every begin/write/end sequence -- from either task -- goes
// through g_udp_mux. Sharing the socket also means one subscription covers both
// streams: whoever sends "BV1 SUB" gets video and attitude, and stops getting
// both 2 s after the last subscribe packet.
//
// The video task lives on core 0 with the HTTP and audio tasks (see the core map
// in main.cpp), and the IMU sender sits one priority below it. Neither is
// coupled to the HTTP servers.

#include "video_udp.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "imu.h"

namespace video_udp {
namespace {

constexpr uint16_t kPort = 82;
constexpr size_t kHeaderBytes = 20;
constexpr size_t kPayloadBytes = 1400;
constexpr uint16_t kMagic = 0x4256;  // little-endian: bytes 'V', 'B'
constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypeVideo = 1;
constexpr uint8_t kTypeImu = 2;
constexpr uint32_t kSubscriberTimeoutMs = 2000;
constexpr char kSubscribePacket[] = "BV1 SUB";
constexpr size_t kSubscribeLen = sizeof(kSubscribePacket) - 1;

// 50 Hz is for a person reading a horizon, and it is 4x the HTTP poll it
// replaces. Everything above that is airtime nobody looks at.
constexpr uint32_t kImuHz = 50;

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

// The sampler's struct IS the wire format: the Godot client decodes fixed
// offsets, so a field added or reordered here has to be a deliberate protocol
// change, not something that quietly shifts a value by four bytes.
constexpr size_t kImuBytes = sizeof(ImuSample);
static_assert(sizeof(ImuSample) == 44, "ImuSample layout is the UDP payload");

WiFiUDP g_udp;
// One transmit buffer, two tasks. Every socket call is serialised through this.
SemaphoreHandle_t g_udp_mux = nullptr;
bool g_active = false;
uint32_t g_frame_id = 0;
IPAddress g_client_ip;
uint16_t g_client_port = 0;
uint32_t g_last_subscribe_ms = 0;
uint32_t g_frames_sent = 0;
uint32_t g_fragments_sent = 0;

void note_subscriber(uint32_t now_ms) {
  // Called with g_udp_mux held: the IMU task reads these fields.
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
  if (xSemaphoreTake(g_udp_mux, pdMS_TO_TICKS(20)) != pdTRUE) return;
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
  xSemaphoreGive(g_udp_mux);
}

// Where to send, or false when nobody is subscribed. Expiring a client that
// stopped subscribing happens here, so whichever sender asks first logs it once
// -- and the IMU task notices it even during a video stall.
//
// The address is copied under the lock because an IPAddress is more than one
// word: the IMU task must not read it while note_subscriber() rewrites it.
// Both out-parameters may be null when only the "is anyone there?" matters.
bool subscriber_address(IPAddress *ip, uint16_t *port) {
  if (xSemaphoreTake(g_udp_mux, pdMS_TO_TICKS(20)) != pdTRUE) return false;

  const uint32_t now = millis();
  if (g_active &&
      (uint32_t)(now - g_last_subscribe_ms) > kSubscriberTimeoutMs) {
    Serial.printf("[udp] subscriber %s:%u timed out\n",
                  g_client_ip.toString().c_str(), (unsigned)g_client_port);
    g_active = false;
  }

  const bool active = g_active;
  if (active) {
    if (ip != nullptr) *ip = g_client_ip;
    if (port != nullptr) *port = g_client_port;
  }

  xSemaphoreGive(g_udp_mux);
  return active;
}

// One datagram, for either payload type. `wait` bounds how long the caller will
// queue behind the other task: the video path waits, because a frame missing one
// fragment is a wasted frame; the IMU path does not, because a sample that
// cannot go out now is already too old to be worth sending.
bool send_datagram(const IPAddress &ip, uint16_t port, const Header &header,
                   const uint8_t *payload, size_t len, TickType_t wait) {
  if (xSemaphoreTake(g_udp_mux, wait) != pdTRUE) return false;

  bool sent = false;
  if (g_udp.beginPacket(ip, port)) {
    const size_t wrote_header =
        g_udp.write(reinterpret_cast<const uint8_t *>(&header), kHeaderBytes);
    const size_t wrote_payload = g_udp.write(payload, len);
    // endPacket() must always follow beginPacket(): it frees the transmit
    // buffer. A short write leaves the datagram malformed (payload_len > the
    // datagram size), so it is abandoned rather than sent.
    const bool complete = g_udp.endPacket();
    sent = wrote_header == kHeaderBytes && wrote_payload == len && complete;
  }

  xSemaphoreGive(g_udp_mux);
  return sent;
}

// Sends one JPEG as ceil(len/payload) self-describing datagrams. Stops early if
// the client disappears mid-frame, so the next frame is not delayed by a
// stream nobody is reading.
void send_frame(const camera_fb_t *fb, const IPAddress &ip, uint16_t port) {
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

    // portMAX_DELAY: losing one fragment costs the whole frame, so this path
    // waits for the socket instead of skipping.
    if (!send_datagram(ip, port, header, fb->buf + offset, len,
                       portMAX_DELAY)) {
      break;
    }
    sent++;

    // Poll every 8 fragments: cheap, and it notices a vanishing client within
    // a frame instead of after the full 2 s timeout.
    if ((i & 7) == 7) {
      poll_subscriptions(millis());
      if (!subscriber_address(nullptr, nullptr)) break;
    }
  }

  g_frames_sent++;
  g_fragments_sent += sent;
}

// Sends the newest sample at kImuHz. Two things are deliberate:
//
//   * imu_latest() copies out of the sampler's critical section and never reads
//     the bus -- the same contract the HTTP handler works to, for the same
//     reason: the sampler task owns the timing.
//   * the sample goes out whole or not at all. A datagram that misses its slot
//     is dropped; the next one carries the then-newest value.
void imu_task(void *) {
  const TickType_t period = pdMS_TO_TICKS(1000 / kImuHz);
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t mark = millis();
  uint32_t mark_samples = 0;
  uint32_t samples = 0;

  while (true) {
    // Also service the subscription here. The video task can sit inside the
    // camera driver for a whole frame; a client that is still sending its
    // keepalive must not be expired -- or lose its attitude -- because of it.
    poll_subscriptions(millis());

    IPAddress client_ip;
    uint16_t client_port = 0;
    // No sensor, no samples: sending zeros would draw a perfectly level horizon
    // and say nothing is wrong. The client expires the last sample instead and
    // shows that the IMU is unavailable.
    if (imu_present() && subscriber_address(&client_ip, &client_port)) {
      ImuSample sample;
      imu_latest(&sample);

      Header header = {};
      header.magic = kMagic;
      header.version = kVersion;
      header.type = kTypeImu;
      // Not a frame: carry the sampler's own sequence so a client can tell a
      // stopped sampler from a car standing still.
      header.frame_id = sample.seq;
      header.total_len = (uint32_t)kImuBytes;
      header.frag_index = 0;
      header.frag_count = 1;
      header.payload_len = (uint16_t)kImuBytes;

      // One slot of patience. If the socket is busy this long, the value the
      // next slot will read is more current than this one.
      if (send_datagram(client_ip, client_port, header,
                        reinterpret_cast<const uint8_t *>(&sample), kImuBytes,
                        period)) {
        samples++;
        mark_samples++;
      }
    }

    const uint32_t now = millis();
    if ((uint32_t)(now - mark) >= 1000) {
      // Silent while nobody is watching: one line a second from an idle task
      // would bury the log the video and HTTP paths are read from.
      if (mark_samples > 0) {
        Serial.printf("[udp] imu %u Hz  samples %u\n",
                      (unsigned)(mark_samples * 1000u / (now - mark)),
                      (unsigned)samples);
      }
      mark = now;
      mark_samples = 0;
    }

    vTaskDelayUntil(&last_wake, period);
  }
}

void task(void *) {
  uint32_t fps_mark = millis();
  uint32_t fps_frames = 0;

  while (true) {
    poll_subscriptions(millis());

    IPAddress client_ip;
    uint16_t client_port = 0;
    if (!subscriber_address(&client_ip, &client_port)) {
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
      send_frame(fb, client_ip, client_port);
      fps_frames++;
    }
    esp_camera_fb_return(fb);

    const uint32_t after = millis();
    if (after - fps_mark >= 1000) {
      const uint32_t fps = fps_frames * 1000u / (after - fps_mark);
      Serial.printf("[udp] %u fps  frames %u  frags %u  -> %s:%u\n",
                    (unsigned)fps, (unsigned)g_frames_sent,
                    (unsigned)g_fragments_sent, client_ip.toString().c_str(),
                    (unsigned)client_port);
      fps_mark = after;
      fps_frames = 0;
    }
  }
}

}  // namespace

bool start() {
  // Bind once, here, instead of inside the video task: both tasks send through
  // this one socket, and neither may run before it exists.
  if (!g_udp.begin(kPort)) {
    Serial.printf("[udp] FAILED to bind UDP port %u\n", (unsigned)kPort);
    return false;
  }

  g_udp_mux = xSemaphoreCreateMutex();
  if (g_udp_mux == nullptr) {
    Serial.println("[udp] FAILED to create the socket mutex -- heap?");
    return false;
  }

  // Video at 4, IMU at 3, both below the httpd task on this core (5). A late
  // frame costs one frame; a late sample costs 20 ms of horizon, and neither is
  // allowed to delay the other's socket write by more than one datagram.
  const BaseType_t video_ok = xTaskCreatePinnedToCore(
      task, "video_udp", 6144, nullptr, 4, nullptr, 0);
  const BaseType_t imu_ok =
      xTaskCreatePinnedToCore(imu_task, "imu_udp", 3072, nullptr, 3, nullptr, 0);
  if (video_ok != pdPASS || imu_ok != pdPASS) return false;

  Serial.printf("[udp] on :%u/udp -- video (newest frame) + IMU (%u Hz)\n",
                (unsigned)kPort, (unsigned)kImuHz);
  Serial.println("[udp] waiting for a subscriber; send \"BV1 SUB\"");
  return true;
}

}  // namespace video_udp
