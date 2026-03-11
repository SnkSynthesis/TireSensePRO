/***********************************************************************
   TPMS MASTER RECEIVER + DISPLAY
   -------------------------------------------------------------
   IMPORTANT NOTES
   -------------------------------------------------------------
   The receiver packets contain pressure + sensor temperature.
   We ONLY use the pressure values.
   Tire temperature will come from an external IR sensor array connected
   via I2C. Those values are represented by irTemps[] placeholders.
***********************************************************************/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "panda.h"   // background car image

#define BTN_OUT 21
#define BTN_IN  3

// -------------------- MODES --------------------
enum Mode {
  MODE_PARK = 0,
  MODE_RACE = 1
};

volatile Mode g_mode = MODE_PARK;

// -------------------- BUTTON DEBOUNCE --------------------
bool lastReading = LOW;
bool stableState = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 40;

// -------------------- ESPNOW CONFIG --------------------
static const uint8_t CHANNEL = 1;

// Node -> Master packet
typedef struct __attribute__((packed)) {
  uint8_t  node_id;
  uint32_t seq;
  uint16_t pressure_x10;
  int16_t  temp_c_x100;   // ignored on display
  uint32_t t_ms;
} Packet;

// Master -> Node packet
typedef struct __attribute__((packed)) {
  uint8_t  for_node_id;
  uint8_t  mode;
  uint16_t next_sleep_s;
  uint32_t echo_seq;
} CommandPacket;

static const uint16_t PARK_INTERVAL_S = 90;
static const uint16_t RACE_INTERVAL_S = 10;

// -------------------- NODE STATS --------------------
struct NodeStats {
  uint32_t last_seq = 0;
  bool     seen = false;
  uint32_t rx_count = 0;
  uint32_t drop_count = 0;
  uint32_t last_rx_ms = 0;

  bool     has_sensor = false;
  float    last_psi = 0.0f;
};

NodeStats stats[5];   // index 1..4

// -------------------- DISPLAY GLOBALS --------------------
TFT_eSPI tft = TFT_eSPI();

const int SCREEN_W = 320;
const int SCREEN_H = 240;

struct Pos { int16_t x, y; };
Pos tire_info_pos[4];

// Pressure from nodes
float tirePressure[4] = {0, 0, 0, 0};

// Placeholder IR temps
float irTemps[4] = {0, 0, 0, 0};

// Flag so loop can redraw after new packet
volatile bool displayDirty = true;

// -------------------- HELPERS --------------------
void printMac(const uint8_t *mac) {
  if (!mac) {
    Serial.print("(null)");
    return;
  }
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static inline void update_seq_stats(NodeStats &s, uint32_t seq) {
  s.rx_count++;
  s.last_rx_ms = millis();

  if (s.seen) {
    if (seq > s.last_seq + 1) {
      s.drop_count += (seq - s.last_seq - 1);
    }
  } else {
    s.seen = true;
  }

  s.last_seq = seq;
}

bool ensure_peer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;      // use current Wi-Fi channel
  peer.encrypt = false;

  esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    Serial.print("esp_now_add_peer failed for ");
    printMac(mac);
    Serial.printf(" err=%d\n", err);
    return false;
  }
  return true;
}

// -------------------- DISPLAY LAYOUT --------------------
void setupPositions() {
  tire_info_pos[0] = {58, 40};    // front left
  tire_info_pos[1] = {262, 40};   // front right
  tire_info_pos[2] = {58, 200};   // rear left
  tire_info_pos[3] = {262, 200};  // rear right
}

bool tjpg_callback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

void drawBackground() {
  tft.fillScreen(TFT_BLACK);

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(tjpg_callback);
  TJpgDec.setSwapBytes(true);

  TJpgDec.drawJpg(0, 20, img_asset, sizeof(img_asset));
}

void drawBox(int16_t cx, int16_t cy, const char* line1, const char* line2, uint16_t color) {
  const int BOX_W = 120;
  const int BOX_H = 55;

  int16_t x = cx - BOX_W / 2;
  int16_t y = cy - BOX_H / 2;

  tft.fillRoundRect(x, y, BOX_W, BOX_H, 8, TFT_BLACK);
  tft.fillRoundRect(x + 3, y + 3, BOX_W - 6, BOX_H - 6, 8, color);

  tft.setTextColor(TFT_BLACK, color);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);

  tft.drawString(line1, cx, y + 16);
  tft.drawString(line2, cx, y + 36);
}

void drawModeIndicator() {
  const int BOX_W = 40;
  const int BOX_H = 30;

  int cx = SCREEN_W / 2;
  int cy = SCREEN_H - 12;

  int x = cx - BOX_W / 2;
  int y = cy - BOX_H / 2;

  tft.fillRoundRect(x, y, BOX_W, BOX_H, 6, TFT_BLACK);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);

  if (g_mode == MODE_RACE) tft.drawString("R", cx + 5, cy);
  else                     tft.drawString("P", cx + 5, cy);
}

void updateDisplay() {
  for (int id = 0; id < 4; id++) {
    char line1[20];
    char line2[20];

    snprintf(line1, sizeof(line1), "%.1f psi", tirePressure[id]);
    snprintf(line2, sizeof(line2), "%.1f C", irTemps[id]);

    uint16_t color = TFT_GREEN;
    if (tirePressure[id] < 29.0f)      color = TFT_RED;
    else if (tirePressure[id] < 31.0f) color = TFT_YELLOW;

    drawBox(tire_info_pos[id].x, tire_info_pos[id].y, line1, line2, color);
  }

  drawModeIndicator();
}

// -------------------- BUTTON --------------------
void checkModeButton() {
  bool reading = digitalRead(BTN_IN);

  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != stableState) {
      stableState = reading;

      if (stableState == HIGH) {
        if (g_mode == MODE_PARK) g_mode = MODE_RACE;
        else                     g_mode = MODE_PARK;

        drawModeIndicator();
        Serial.printf("Mode changed to %s\n", (g_mode == MODE_RACE) ? "RACE" : "PARK");
      }
    }
  }

  lastReading = reading;
}

// -------------------- ESPNOW CALLBACKS --------------------
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  Serial.print("TX reply status=");
  Serial.print(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");

  if (tx_info) {
    Serial.print(" | dest=");
    printMac(tx_info->des_addr);
  }
  Serial.println();
}

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data) return;

  if (len != (int)sizeof(Packet)) {
    Serial.printf("RX bad len=%d from ", len);
    printMac(info->src_addr);
    Serial.println();
    return;
  }

  Packet p;
  memcpy(&p, data, sizeof(p));

  if (p.node_id < 1 || p.node_id > 4) return;

  NodeStats &s = stats[p.node_id];
  update_seq_stats(s, p.seq);

  if (p.pressure_x10 != 0xFFFF) {
    s.last_psi = p.pressure_x10 / 10.0f;
    s.has_sensor = true;
    tirePressure[p.node_id - 1] = s.last_psi;
    displayDirty = true;
  } else {
    s.has_sensor = false;
  }

  Serial.print("RX from ");
  printMac(info->src_addr);
  Serial.printf(" | node=%u seq=%lu psi=%.1f\n",
                p.node_id,
                (unsigned long)p.seq,
                s.last_psi);

  CommandPacket cmd;
  cmd.for_node_id = p.node_id;
  cmd.mode = g_mode;
  cmd.next_sleep_s = (g_mode == MODE_RACE) ? RACE_INTERVAL_S : PARK_INTERVAL_S;
  cmd.echo_seq = p.seq;

  if (ensure_peer(info->src_addr)) {
    esp_err_t err = esp_now_send(info->src_addr, (uint8_t*)&cmd, sizeof(cmd));
    if (err != ESP_OK) {
      Serial.printf("esp_now_send reply failed err=%d\n", err);
    }
  }
}

// -------------------- SETUP --------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  // ---- DISPLAY INIT FIRST OR LAST is usually fine; keep it simple here ----
  tft.init();
  tft.setRotation(1);

  setupPositions();
  drawBackground();
  drawModeIndicator();

  pinMode(BTN_OUT, OUTPUT);
  digitalWrite(BTN_OUT, HIGH);

  pinMode(BTN_IN, INPUT);

  // ---- Wi-Fi / ESPNOW init: same pattern as the working master ----
  WiFi.mode(WIFI_STA);
  delay(200);

  esp_err_t err = esp_wifi_start();
  Serial.printf("esp_wifi_start err=%d\n", err);

  err = esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("esp_wifi_set_ps err=%d\n", err);

  err = esp_wifi_set_promiscuous(true);
  Serial.printf("set_promiscuous(true): %d\n", err);

  err = esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("set_channel(%u): %d\n", CHANNEL, err);

  err = esp_wifi_set_promiscuous(false);
  Serial.printf("set_promiscuous(false): %d\n", err);

  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  err = esp_wifi_get_channel(&primary, &second);
  Serial.printf("get_channel: err=%d primary=%u\n", err, primary);

  Serial.print("Master MAC: ");
  Serial.println(WiFi.macAddress());

  err = esp_now_init();
  Serial.printf("esp_now_init: %d\n", err);
  if (err != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onDataSent);

  Serial.println("Integrated master/display ready");
}

// -------------------- LOOP --------------------
void loop() {
  checkModeButton();

  static uint32_t lastDisplay = 0;
  if (displayDirty || (millis() - lastDisplay > 500)) {
    lastDisplay = millis();
    displayDirty = false;
    updateDisplay();
  }

  static uint32_t lastStats = 0;
  if (millis() - lastStats >= 1000) {
    lastStats = millis();

    Serial.print("---- Stats ---- | mode=");
    Serial.println((g_mode == MODE_RACE) ? "RACE" : "PARK");

    for (int id = 1; id <= 4; id++) {
      NodeStats &s = stats[id];
      uint32_t age = s.seen ? (millis() - s.last_rx_ms) : 999999UL;

      if (s.has_sensor) {
        Serial.printf("Node %d: rx=%lu drops=%lu age=%lums psi=%.1f\n",
                      id,
                      (unsigned long)s.rx_count,
                      (unsigned long)s.drop_count,
                      (unsigned long)age,
                      s.last_psi);
      } else {
        Serial.printf("Node %d: rx=%lu drops=%lu age=%lums (no data)\n",
                      id,
                      (unsigned long)s.rx_count,
                      (unsigned long)s.drop_count,
                      (unsigned long)age);
      }
    }
  }
}
