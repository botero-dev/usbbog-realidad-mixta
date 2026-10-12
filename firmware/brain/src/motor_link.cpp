// motor_link.cpp — the brain's half of the link. See motor_link.h.
// ---------------------------------------------------------------------------

#include "motor_link.h"

#include <Arduino.h>

#include "mixer.h"

namespace motor_link {
namespace {

constexpr int kPinTx = 1;  // -> Pico GP17 (UART0 RX)
constexpr int kPinRx = 2;  // <- Pico GP16 (UART0 TX)

constexpr uint32_t kLogPeriodMs = 5000;

// --- command state: written by HTTP handlers, read by the link task ---------
//
// Guarded by a spinlock rather than made atomic, because the four duties plus
// the mode are five separate values and a reader must not see half of one
// command and half of the next. The critical sections are a handful of
// instructions, so the lock costs nothing at these rates.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_mode = proto::MSG_DISABLE;
int16_t g_duty[proto::MOTOR_COUNT] = {0, 0, 0, 0};
uint32_t g_last_cmd_ms = 0;

// --- link state: owned by the link task only -------------------------------
// Nothing outside the task touches these, so they need no lock. The getters at
// the bottom read them as plain words, which is a benign race: every one of
// them is a single 32-bit value that a reader either sees just before or just
// after an update, never a torn mixture.
uint8_t g_seq = 0;
uint32_t g_tx_frames = 0;

uint8_t g_win[proto::REPLY_LEN];  // sliding window, same trick as the Pico side
proto::SyncHunt g_hunt(g_win, proto::REPLY_LEN, proto::reply_ok);
uint32_t g_rx_replies = 0;
uint32_t g_rx_crc_errors = 0;

bool g_pico_seen = false;
uint8_t g_pico_status = 0;
uint8_t g_pico_fault = 0;
uint8_t g_prev_status = 0;
uint8_t g_prev_fault = 0;
uint16_t g_pico_uptime_ms = 0;
uint32_t g_pico_last_ms = 0;

bool g_timeout_active = false;

int16_t clamp_input(int v) {
  if (v > proto::DUTY_MAX) return proto::DUTY_MAX;
  if (v < -proto::DUTY_MAX) return -proto::DUTY_MAX;
  return static_cast<int16_t>(v);
}

// Write a command and stamp the watchdog. `duty` may be null for the modes that
// do not use it, in which case the stored duties are zeroed -- so that
// state().duty always describes what is actually being commanded, and a later
// SET cannot inherit a stale value.
void set_command(uint8_t mode, const int16_t* duty) {
  portENTER_CRITICAL(&g_mux);
  g_mode = mode;
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
    g_duty[i] = duty ? duty[i] : 0;
  }
  g_last_cmd_ms = millis();
  portEXIT_CRITICAL(&g_mux);
}

// --- reply parsing ---------------------------------------------------------

const char* fault_text(uint8_t fault) {
  switch (fault) {
    case proto::FAULT_OK:      return "ok";
    case proto::FAULT_CRC:     return "crc errors at the Pico";
    case proto::FAULT_TIMEOUT: return "DEAD-MAN FIRED at the Pico";
    case proto::FAULT_HW:      return "hardware fault";
    default:                   return "unknown";
  }
}

void status_text(uint8_t s, char* out, size_t out_len) {
  snprintf(out, out_len, "stby=%u link=%u wdt=%u", (s & proto::ST_STBY_HIGH) ? 1u : 0u,
           (s & proto::ST_LINK_GOOD) ? 1u : 0u, (s & proto::ST_WDT_ARMED) ? 1u : 0u);
}

// Log only TRANSITIONS, because these lines are evidence and a repeating line is
// not evidence. The periodic summary goes out in the heartbeat instead.
void note_reply(const proto::Reply& r) {
  g_rx_replies++;
  g_pico_uptime_ms = r.uptime_ms;
  g_pico_last_ms = millis();

  char bits[48];
  status_text(r.status, bits, sizeof(bits));

  if (!g_pico_seen) {
    // The single most useful line of the whole bring-up: if this never appears,
    // nothing downstream of it can be trusted, and nothing upstream of it needs
    // looking at.
    g_pico_seen = true;
    g_pico_status = g_prev_status = r.status;
    g_pico_fault = g_prev_fault = r.fault;
    Serial.printf("[link] FIRST reply from the Pico: status 0x%02x (%s) fault %u (%s) uptime %u ms\n",
                  r.status, bits, r.fault, fault_text(r.fault), r.uptime_ms);
    return;
  }

  g_pico_status = r.status;
  if (r.status != g_prev_status) {
    Serial.printf("[link] pico status 0x%02x -> 0x%02x (%s)\n", g_prev_status,
                  r.status, bits);
    g_prev_status = r.status;
  }

  g_pico_fault = r.fault;
  if (r.fault != g_prev_fault) {
    if (r.fault != proto::FAULT_OK) {
      // FAULT_TIMEOUT deserves the shouting: if the Pico's dead-man fired, the
      // Pico stopped receiving frames, which means this end stopped sending
      // them -- a task that stalled, a crash, a wire. It is not the Pico's
      // fault and should not be debugged there.
      Serial.printf("[link] *** PICO FAULT %u: %s ***\n", r.fault,
                    fault_text(r.fault));
    } else {
      Serial.printf("[link] pico fault cleared (was %u)\n", g_prev_fault);
    }
    g_prev_fault = r.fault;
  }
}

void poll_replies() {
  while (Serial1.available() > 0) {
    const uint8_t b = static_cast<uint8_t>(Serial1.read());

    if (!g_hunt.feed(b)) {
      // Same split as the Pico side: only a sync pattern that FAILED its CRC is
      // worth counting. Plain noise is not an error, it is a line with nothing
      // on it -- and with the link on UART1 there is no ROM banner to explain.
      if (proto::sync_ok(g_hunt.window())) g_rx_crc_errors++;
      continue;
    }

    proto::Reply r;
    proto::reply_decode(g_hunt.window(), &r);
    note_reply(r);
  }
}

// --- the 100 Hz task ------------------------------------------------------

void task(void* /*arg*/) {
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / kFrameHz);

  for (;;) {
    // vTaskDelayUntil, not vTaskDelay: the Pico's dead-man does not care that a
    // previous frame took longer than usual, so the frame rate has to be driven
    // by a schedule rather than by how long the last iteration happened to take.
    vTaskDelayUntil(&last_wake, period);

    const uint32_t now = millis();
    uint8_t type;
    int16_t duty[proto::MOTOR_COUNT];

    portENTER_CRITICAL(&g_mux);
    type = g_mode;
    for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) duty[i] = g_duty[i];
    const uint32_t last_cmd = g_last_cmd_ms;
    portEXIT_CRITICAL(&g_mux);

    // The control watchdog. Only relevant while we are commanding duties: an
    // explicit BRAKE/COAST/DISABLE is a resting state and must not expire.
    // See the long note in motor_link.h on why the Pico's dead-man cannot cover
    // this case.
    const bool timed_out =
        (type == proto::MSG_SET_DUTIES) &&
        (static_cast<uint32_t>(now - last_cmd) > kControlTimeoutMs);
    if (timed_out) type = proto::MSG_BRAKE;

    if (timed_out != g_timeout_active) {
      g_timeout_active = timed_out;
      if (timed_out) {
        Serial.printf("[link] CONTROL TIMEOUT: no drive command for %u ms -> BRAKE\n",
                      (unsigned)kControlTimeoutMs);
      } else {
        Serial.println("[link] control timeout cleared");
      }
    }

    uint8_t f[proto::TX_FRAME_LEN];
    proto::frame_encode(f, type, g_seq++, duty);
    // Blocking write into the driver's TX buffer. At 13 bytes per 10 ms the
    // buffer is never near full, and this task has 10 ms of slack before the
    // next frame is due, so it never delays the schedule meaningfully.
    Serial1.write(f, proto::TX_FRAME_LEN);
    g_tx_frames++;

    poll_replies();
  }
}

}  // namespace

bool init() {
  Serial1.begin(kBaud, SERIAL_8N1, kPinRx, kPinTx);

  const BaseType_t ok = xTaskCreatePinnedToCore(task, "motor_link", 4096,
                                                nullptr, 5, nullptr, 1);
  if (ok != pdPASS) return false;

  Serial.printf("[link] UART1 up: %u baud 8N1, TX GPIO%d -> Pico GP17, RX GPIO%d <- Pico GP16\n",
                (unsigned)kBaud, kPinTx, kPinRx);
  Serial.printf("[link] sending %u Hz frames; control watchdog %u ms -> BRAKE\n",
                (unsigned)kFrameHz, (unsigned)kControlTimeoutMs);
  Serial.println("[link] starting DISABLED: the drivers stay high-Z until /drive.");
  return true;
}

void drive(int16_t throttle, int16_t front_rear, int16_t left_right) {
  mixer::Input in;
  in.throttle = clamp_input(throttle);
  in.front_rear = clamp_input(front_rear);
  in.left_right = clamp_input(left_right);

  int16_t duty[proto::MOTOR_COUNT];
  mixer::mix(in, duty);
  set_command(proto::MSG_SET_DUTIES, duty);
}

void drive_raw(const int16_t duty[proto::MOTOR_COUNT]) {
  int16_t clamped[proto::MOTOR_COUNT];
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
    clamped[i] = clamp_input(duty[i]);
  }
  set_command(proto::MSG_SET_DUTIES, clamped);
}

void brake() { set_command(proto::MSG_BRAKE, nullptr); }
void coast() { set_command(proto::MSG_COAST, nullptr); }
void disable() { set_command(proto::MSG_DISABLE, nullptr); }

void ping() {
  // PING is a one-shot, not a mode: it must not become the type we keep sending,
  // or the link would stop commanding motion entirely. The 100 Hz task will
  // overwrite it with the current mode on the very next tick, so this is sent
  // once, immediately, out of band.
  uint8_t f[proto::TX_FRAME_LEN];
  const int16_t zero[proto::MOTOR_COUNT] = {0, 0, 0, 0};
  proto::frame_encode(f, proto::MSG_PING, g_seq++, zero);
  Serial1.write(f, proto::TX_FRAME_LEN);
}

void state(State* out) {
  portENTER_CRITICAL(&g_mux);
  out->mode = g_mode;
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) out->duty[i] = g_duty[i];
  portEXIT_CRITICAL(&g_mux);

  out->pico_seen = g_pico_seen;
  out->pico_status = g_pico_status;
  out->pico_fault = g_pico_fault;
  out->pico_uptime_ms = g_pico_uptime_ms;
  out->pico_age_ms = g_pico_seen ? (millis() - g_pico_last_ms) : 0xFFFFFFFFu;
  out->tx_frames = g_tx_frames;
  out->rx_replies = g_rx_replies;
  out->rx_crc_errors = g_rx_crc_errors;
  out->control_timeout = g_timeout_active;
}

const char* mode_name(uint8_t type) {
  switch (type) {
    case proto::MSG_SET_DUTIES: return "SET";
    case proto::MSG_BRAKE:      return "BRAKE";
    case proto::MSG_COAST:      return "COAST";
    case proto::MSG_DISABLE:    return "DISABLE";
    case proto::MSG_PING:       return "PING";
    default:                    return "?";
  }
}

}  // namespace motor_link
