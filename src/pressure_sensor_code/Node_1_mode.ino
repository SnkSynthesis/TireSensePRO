#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include "esp_sleep.h"

static const int CHANNEL = 1;

static const uint8_t NODE_ID = 1;
uint8_t MASTER_MAC[] = { 0xF0, 0x24, 0xF9, 0xE8, 0x53, 0xA4 };

enum : uint8_t { MODE_PARK = 0, MODE_RACE = 1 };

// Intervals (sec)
static const uint32_t PARK_INTERVAL_S = 90;
static const uint32_t RACE_INTERVAL_S = 10;

// Receive window after send (ms)
static const uint32_t CMD_WINDOW_MS = 150;

// This is if we miss master reply this many wakes in a row we force PARK
static const uint8_t MISS_TO_FORCE_PARK = 3;

// ABP2 sensor I2C
static const uint8_t ABP2_ADDR = 0x28;
static const uint8_t ABP2_CMD[3] = { 0xAA, 0x00, 0x00 };
static uint8_t data_buf[7];

const double OUTPUTMAX = 15099494.0;
const double OUTPUTMIN = 1677722.0;
const double PMAX = 100.0;
const double PMIN = 0.0;

// Stagger offsets to reduce collisions (optional)
uint32_t offset_ms_for_node(uint8_t id) {
  switch (id) {
    case 1: return 0;
    case 2: return 12;
    case 3: return 24;
    case 4: return 36;
    default: return 0;
  }
}

// must MATCH master exactly
typedef struct __attribute__((packed)) {
  uint8_t  node_id;
  uint32_t seq;
  uint16_t pressure_x10;   // psi * 10
  int16_t  temp_c_x100;    // C * 100
  uint32_t t_ms;
} UplinkPacket;

// Master -> Node command
typedef struct __attribute__((packed)) {
  uint8_t  for_node_id;
  uint8_t  mode;
  uint16_t next_sleep_s;
  uint32_t echo_seq;
} CommandPacket;

// RTC State
RTC_DATA_ATTR uint32_t seq = 0;
RTC_DATA_ATTR uint16_t next_sleep_s = PARK_INTERVAL_S;
RTC_DATA_ATTR uint8_t  mode = MODE_PARK;
RTC_DATA_ATTR uint8_t  miss_count = 0;

// Call back state
volatile bool send_done = false;
volatile esp_now_send_status_t send_status = ESP_NOW_SEND_FAIL;

volatile bool cmd_received = false;
CommandPacket last_cmd = {};

// callbacks
void onSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  (void)tx_info;
  send_status = status;
  send_done = true;
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  if (len != (int)sizeof(CommandPacket)) return;

  CommandPacket c;
  memcpy(&c, data, sizeof(c));

  if (c.for_node_id != NODE_ID) return;
  if (c.mode != MODE_PARK && c.mode != MODE_RACE) return;
  if (c.next_sleep_s == 0) return;

  last_cmd = c;
  cmd_received = true;
}

// read sensor
bool readABP2(double &pressure_psi, double &temp_c) {
  Wire.beginTransmission(ABP2_ADDR);
  Wire.write(ABP2_CMD, 3);
  if (Wire.endTransmission() != 0) return false;

  delay(10);

  if (Wire.requestFrom((int)ABP2_ADDR, 7) != 7) return false;
  for (int i = 0; i < 7; i++) data_buf[i] = Wire.read();

  uint32_t pressure_counts = ((uint32_t)data_buf[1] << 16) |
                            ((uint32_t)data_buf[2] << 8)  |
                            (uint32_t)data_buf[3];

  uint32_t temp_counts = ((uint32_t)data_buf[4] << 16) |
                         ((uint32_t)data_buf[5] << 8)  |
                         (uint32_t)data_buf[6];

  temp_c = (temp_counts * 200.0 / 16777215.0) - 50.0;

  pressure_psi = ((pressure_counts - OUTPUTMIN) * (PMAX - PMIN)) /
                 (OUTPUTMAX - OUTPUTMIN) + PMIN;

  return true;
}

// sleep
static void go_to_sleep(uint16_t sleep_s) {
  esp_now_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * 1000000ULL);
  delay(20);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  uint32_t wake_start_ms = millis();
  delay(offset_ms_for_node(NODE_ID));

  Wire.begin();

  // Read sensor once
  double psi = 0.0, tempC = 0.0;
  bool ok = readABP2(psi, tempC);

  uint16_t p_x10 = 0xFFFF;
  int16_t  t_x100 = 0x7FFF;

  if (ok) {
    if (psi < 0) psi = 0;
    p_x10 = (uint16_t)(psi * 10.0 + 0.5);

    double tc = tempC;
    if (tc < -327.68) tc = -327.68;
    if (tc > 327.67)  tc = 327.67;
    t_x100 = (int16_t)(tc * 100.0);
  }

  // Bring up ESPNOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Force Wi-Fi channel
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed -> sleeping");
    go_to_sleep(PARK_INTERVAL_S);
  }

  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  // add master as peer
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MASTER_MAC, 6);
  peer.channel = CHANNEL;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add master peer -> sleeping");
    go_to_sleep(PARK_INTERVAL_S);
  }

  // Build uplink, must match master
  UplinkPacket up;
  up.node_id = NODE_ID;
  up.seq = seq++;
  up.pressure_x10 = p_x10;
  up.temp_c_x100  = t_x100;
  up.t_ms = millis();

  // Send uplink
  send_done = false;
  cmd_received = false;

  esp_err_t r = esp_now_send(MASTER_MAC, (uint8_t*)&up, sizeof(up));
  if (r != ESP_OK) {
    Serial.printf("esp_now_send err=%d -> sleeping\n", (int)r);
    miss_count++;
    if (miss_count >= MISS_TO_FORCE_PARK) {
      mode = MODE_PARK;
      next_sleep_s = PARK_INTERVAL_S;
      miss_count = 0;
    }
    go_to_sleep(next_sleep_s);
  }

  // Wait for send callback
  uint32_t t0 = millis();
  while (!send_done && (millis() - t0) < 200) delay(1);

  // cmd receive window
  uint32_t t_cmd = millis();
  while (!cmd_received && (millis() - t_cmd) < CMD_WINDOW_MS) delay(1);

  if (cmd_received) {
    mode = last_cmd.mode;
    next_sleep_s = last_cmd.next_sleep_s;
    miss_count = 0;
  } else {
    miss_count++;
    if (miss_count >= MISS_TO_FORCE_PARK) {
      mode = MODE_PARK;
      next_sleep_s = PARK_INTERVAL_S;
      miss_count = 0;
    }
  }

  // Clamp intervals
  if (next_sleep_s < 5) next_sleep_s = 5;
  if (next_sleep_s > 600) next_sleep_s = 600;

  // Debug, plan to remove when finalized
  Serial.printf("Node %u psi=%.2f temp=%.2fC ok=%d\n", NODE_ID, psi, tempC, ok ? 1 : 0);
  Serial.printf("Send=%s | cmd=%s | mode=%s | next_sleep=%us | wake_window=%lums\n",
                (send_done && send_status == ESP_NOW_SEND_SUCCESS) ? "OK" : "FAIL/TO",
                cmd_received ? "YES" : "NO",
                (mode == MODE_RACE) ? "RACE" : "PARK",
                (unsigned)next_sleep_s,
                (unsigned long)(millis() - wake_start_ms));

  go_to_sleep(next_sleep_s);
}

void loop() {}
