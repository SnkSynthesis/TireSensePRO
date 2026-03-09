/***********************************************************************
   TPMS MASTER RECEIVER + DISPLAY
   -------------------------------------------------------------
   IMPORTANT NOTES
   -------------------------------------------------------------
   The receiver packets contain pressure + sensor temperature.
   We ONLY use the pressure values.
   Tire temperature will come from an external IR sensor array connected via I2C. Those values are represented by irTemps[] placeholders.
***********************************************************************/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "panda.h"   // background car image

#define BTN_OUT 21  // pin that will drive HIGH (connects to one side of button)
#define BTN_IN  3   // pin that reads the button (other side of button)

// Modes
enum Mode {
  MODE_PARK = 0,
  MODE_RACE = 1
};

Mode g_mode = MODE_PARK;   // current mode (default)

// Debounce state
bool lastReading = LOW;               // last raw reading from the pin
bool stableState = LOW;               // debounced stable state
unsigned long lastDebounceTime = 0;   // when the last (raw) change was detected
const unsigned long debounceDelay = 40; // milliseconds

static const int CHANNEL = 1;

typedef struct __attribute__((packed)) {

  uint8_t  node_id;        // sensor ID (1-4)
  uint32_t seq;            // packet sequence counter

  uint16_t pressure_x10;   // pressure in PSI * 10
  int16_t  temp_c_x100;    // temperature from sensor (IGNORED for display)

  uint32_t t_ms;

} Packet;

typedef struct __attribute__((packed)) {

  uint8_t  for_node_id;
  uint8_t  mode;
  uint16_t next_sleep_s;
  uint32_t echo_seq;

} CommandPacket;

static const uint16_t PARK_INTERVAL_S = 90;
static const uint16_t RACE_INTERVAL_S = 10;

struct NodeStats {

  uint32_t last_seq = 0;
  bool seen = false;

  uint32_t rx_count = 0;
  uint32_t drop_count = 0;
  uint32_t last_rx_ms = 0;

  bool has_sensor = false;

  double last_psi = 0.0;     // pressure we will display
};

NodeStats stats[5];   // index by node_id 1..4


/***************************************************************
   DISPLAY GLOBALS
***************************************************************/
TFT_eSPI tft = TFT_eSPI();

const int SCREEN_W = 320;
const int SCREEN_H = 240;


/***************************************************************
   TIRE POSITION STRUCT
   Holds where each tire info box should be drawn
***************************************************************/
struct Pos { int16_t x, y; };

Pos tire_info_pos[4];


/***************************************************************
   DISPLAY DATA ARRAYS
***************************************************************/

// pressure values received from sensors
float tirePressure[4] = {0,0,0,0};

// temperature values from IR sensors (I2C input)
float irTemps[4] = {0,0,0,0};


/***************************************************************
   SET DISPLAY POSITIONS
***************************************************************/
void setupPositions() {

  tire_info_pos[0] = {58, 40};    // front left tire (drivers side)
  tire_info_pos[1] = {262, 40};   // front right tire (passenger side)
  tire_info_pos[2] = {58, 200};   // rear left tire (behind driver)
  tire_info_pos[3] = {262, 200};  // rear right tire (behind passenger)

}

bool tjpg_callback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {

  tft.pushImage(x, y, w, h, bitmap);
  return true;

}


/***************************************************************
   DRAW THE SNAZZY CAR IMAGE from panda.h 
***************************************************************/
void drawBackground() {

  tft.fillScreen(TFT_BLACK);

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(tjpg_callback);
  TJpgDec.setSwapBytes(true);

  TJpgDec.drawJpg(0, 20, img_asset, sizeof(img_asset));

}


/***************************************************************
   DRAW TIRE INFO BOXES
***************************************************************/
void drawBox(int16_t cx, int16_t cy, const char* line1, const char* line2, uint16_t color) {

  const int BOX_W = 120;
  const int BOX_H = 55;

  int16_t x = cx - BOX_W/2;
  int16_t y = cy - BOX_H/2;

  tft.fillRoundRect(x, y, BOX_W, BOX_H, 8, TFT_BLACK);
  tft.fillRoundRect(x+3, y+3, BOX_W-6, BOX_H-6, 8, color);

  tft.setTextColor(TFT_BLACK, color);
  tft.setTextSize(2);

  tft.setTextDatum(MC_DATUM);

  tft.drawString(line1, cx, y + 16);
  tft.drawString(line2, cx, y + 36);
}

/*****************************************************************
   PARK/RACE MODE AND INDICATOR
   Displays a white "R" or "P" centered at the bottom of screen.
*****************************************************************/
void checkModeButton() {
  bool reading = digitalRead(BTN_IN);

  // If raw reading changed, reset debounce timer
  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  // If the reading has been stable longer than debounceDelay, treat it as the stable state
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != stableState) {
      stableState = reading;
      // Only toggle on the moment it becomes HIGH (button pressed)
      if (stableState == HIGH) {
        // Toggle mode
        if (g_mode == MODE_PARK) g_mode = MODE_RACE;
        else                    g_mode = MODE_PARK;

        // Update indicator immediately
        drawModeIndicator();
      }
    }
  }

  // save for next loop
  lastReading = reading;
}

void drawModeIndicator()
{
  const int BOX_W = 40;
  const int BOX_H = 30;

  int cx = SCREEN_W / 2;
  int cy = SCREEN_H - 12;

  int x = cx - BOX_W/2;
  int y = cy - BOX_H/2;

  // clear the region first
  tft.fillRoundRect(x, y, BOX_W, BOX_H, 6, TFT_BLACK);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);

  if (g_mode == MODE_RACE)
    tft.drawString("R", cx+5, cy);
  else
    tft.drawString("P", cx+5, cy);
}

/***************************************************************
   UPDATE DISPLAY FROM CURRENT DATA ARRAYS
***************************************************************/
void updateDisplay() {

  for (int id = 0; id < 4; id++) {

    char line1[20];
    char line2[20];

    // format pressure string
    snprintf(line1, sizeof(line1), "%.1f psi", tirePressure[id]);

    // format IR temperature string
    snprintf(line2, sizeof(line2), "%.1f C", irTemps[id]);

    uint16_t color = TFT_GREEN;

    if (tirePressure[id] < 29) color = TFT_RED;
    else if (tirePressure[id] < 31) color = TFT_YELLOW;

    drawBox(tire_info_pos[id].x, tire_info_pos[id].y, line1, line2, color);
  }

}

static inline void update_seq_stats(NodeStats &s, uint32_t seq) {

  s.rx_count++;
  s.last_rx_ms = millis();

  if (s.seen) {

    if (seq > s.last_seq + 1)
      s.drop_count += (seq - s.last_seq - 1);

  } else {
    s.seen = true;
  }

  s.last_seq = seq;

}


bool ensure_peer(const uint8_t mac[6]) {

  if (esp_now_is_peer_exist(mac))
    return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);

  peer.channel = CHANNEL;
  peer.encrypt = false;

  return (esp_now_add_peer(&peer) == ESP_OK);

}

void onReceive(const esp_now_recv_info_t *info,
               const uint8_t *data,
               int len) {

  if (!info || !data) return;

  if (len == sizeof(Packet)) {

    Packet p;
    memcpy(&p, data, sizeof(p));

    if (p.node_id < 1 || p.node_id > 4)
      return;

    NodeStats &s = stats[p.node_id];

    update_seq_stats(s, p.seq);

    // decode pressure
    if (p.pressure_x10 != 0xFFFF) {

      s.last_psi = p.pressure_x10 / 10.0;
      s.has_sensor = true;

      // update display array
      tirePressure[p.node_id - 1] = s.last_psi;
    }

    // build command reply
    CommandPacket cmd;

    cmd.for_node_id = p.node_id;
    cmd.mode = g_mode;
    cmd.next_sleep_s =
      (g_mode == MODE_RACE) ? RACE_INTERVAL_S : PARK_INTERVAL_S;

    cmd.echo_seq = p.seq;

    if (ensure_peer(info->src_addr)) {
      esp_now_send(info->src_addr,
                   (uint8_t*)&cmd,
                   sizeof(cmd));
    }
  }
}


/***************************************************************
   SETUP
***************************************************************/
void setup() {

  Serial.begin(115200);
  delay(200);


  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {

    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);

  }

  esp_now_register_recv_cb(onReceive);

  /***** DISPLAY SETUP *****/

  tft.init();
  tft.setRotation(1);

  setupPositions();
  drawBackground();
  drawModeIndicator();
  pinMode(BTN_OUT, OUTPUT);
  digitalWrite(BTN_OUT, HIGH);   // provide HIGH for button circuit

  pinMode(BTN_IN, INPUT);

}


/***************************************************************
   LOOP
***************************************************************/
void loop() {

  /***** UPDATE DISPLAY CONTINUOUSLY *****/

  static uint32_t lastDisplay = 0;

  if (millis() - lastDisplay > 500) {

    lastDisplay = millis();

    updateDisplay();

  }

  /***** BUTTON MODE TOGGLE *****/
  checkModeButton();

}
