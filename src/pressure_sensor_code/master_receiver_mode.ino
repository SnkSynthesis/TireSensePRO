#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const int CHANNEL = 1;

// packet (must match node side exactly)
typedef struct __attribute__((packed)) {
  uint8_t  node_id;
  uint32_t seq;
  uint16_t pressure_x10;   // psi * 10  (0.1 psi units)
  int16_t  temp_c_x100;    // C * 100   (0.01 C units)
  uint32_t t_ms;
} Packet;

// DOWNLINK COMMAND
typedef struct __attribute__((packed)) {
  uint8_t  for_node_id;    // target node id
  uint8_t  mode;           // 0=PARK, 1=RACE
  uint16_t next_sleep_s;   // seconds
  uint32_t echo_seq;       // last seq master saw from that node
} CommandPacket;

enum : uint8_t { MODE_PARK = 0, MODE_RACE = 1 };

static const uint16_t PARK_INTERVAL_S = 90;
static const uint16_t RACE_INTERVAL_S = 10;

// Global master mode (toggle via Serial: 'p' or 'r')
volatile uint8_t g_mode = MODE_PARK;

// Stats
struct NodeStats {
  uint32_t last_seq = 0;
  bool     seen = false;
  uint32_t rx_count = 0;
  uint32_t drop_count = 0;
  uint32_t last_rx_ms = 0;

  // latest sensor values
  bool     has_sensor = false;
  double   last_psi = 0.0;
  double   last_temp_c = 0.0;
};

NodeStats stats[5]; // index by node_id 1..4

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

// ensure a peer exists before sending to it.
bool ensure_peer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = CHANNEL;
  peer.encrypt = false;

  return (esp_now_add_peer(&peer) == ESP_OK);
}

// ESPNOW rx callback
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data) return;

  // Accept packet
  if (len == (int)sizeof(Packet)) {
    Packet p;
    memcpy(&p, data, sizeof(p));

    if (p.node_id < 1 || p.node_id > 4) return;

    NodeStats &s = stats[p.node_id];
    update_seq_stats(s, p.seq);

    // decode if not sentinel
    if (p.pressure_x10 != 0xFFFF && p.temp_c_x100 != 0x7FFF) {
      s.last_psi = p.pressure_x10 / 10.0;
      s.last_temp_c = p.temp_c_x100 / 100.0;
      s.has_sensor = true;
    } else {
      s.has_sensor = false;
    }

    // Build downlink command
    CommandPacket cmd;
    cmd.for_node_id  = p.node_id;
    cmd.mode         = g_mode;
    cmd.next_sleep_s = (g_mode == MODE_RACE) ? RACE_INTERVAL_S : PARK_INTERVAL_S;
    cmd.echo_seq     = p.seq;

    // Reply to sender MAC (node)
    if (ensure_peer(info->src_addr)) {
      (void)esp_now_send(info->src_addr, (uint8_t*)&cmd, sizeof(cmd));
    }

  } else {
    // Unknown/weird payload size
    return;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("Master MAC: ");
  Serial.println(WiFi.macAddress());

  // Force channel
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onReceive);

  Serial.println("Master ready. Type 'r' for RACE (10s) or 'p' for PARK (90s).");
}

void loop() {
  // Manual mode toggle for testing
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'r' || c == 'R') {
      g_mode = MODE_RACE;
      Serial.println("MASTER MODE = RACE (10s)");
    } else if (c == 'p' || c == 'P') {
      g_mode = MODE_PARK;
      Serial.println("MASTER MODE = PARK (90s)");
    }
  }

  // Stats print
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.print("---- Stats (1s) ---- | MasterMode=");
    Serial.println((g_mode == MODE_RACE) ? "RACE" : "PARK");

    for (int id = 1; id <= 4; id++) {
      NodeStats &s = stats[id];
      uint32_t age = s.seen ? (millis() - s.last_rx_ms) : 999999UL;

      if (s.has_sensor) {
        Serial.printf("Node %d: rx=%lu drops=%lu last_rx=%lums ago | P=%.1f psi T=%.2f C\n",
                      id, (unsigned long)s.rx_count, (unsigned long)s.drop_count,
                      (unsigned long)age, s.last_psi, s.last_temp_c);
      } else {
        Serial.printf("Node %d: rx=%lu drops=%lu last_rx=%lums ago | (no sensor)\n",
                      id, (unsigned long)s.rx_count, (unsigned long)s.drop_count,
                      (unsigned long)age);
      }
    }
  }
}
