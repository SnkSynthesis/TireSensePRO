// TPMS_Display_Test_SwapBytes.ino
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "panda.h"

TFT_eSPI tft = TFT_eSPI();

// Original display is 240x320 (portrait), but rotation makes it 320x240
const int SCREEN_W = 320;
const int SCREEN_H = 240;

// Struct used to store screen positions for each tire information box
// x = horizontal position
// y = vertical position
struct Pos { int16_t x, y; };
Pos tire_info_pos[4]; // Array holding the positions for the 4 tires

// Set the screen positions for each tire information box
void setupPositions() {

  tire_info_pos[0] = {58, 40};    // Front left tire (driver side)
  tire_info_pos[1] = {262, 40};   // Front right tire (passenger side)
  tire_info_pos[2] = {58, 200};   // Rear left tire (behind driver)
  tire_info_pos[3] = {262, 200};  // Rear right tire (behind passenger)

}

// JPEG callback
bool tjpg_callback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// Draws a rounded box that contains the PSI and temperature
void drawBox(int16_t cx, int16_t cy, const char* line1, const char* line2, uint16_t color) {

  const int BOX_W = 120;   // wider box
  const int BOX_H = 55;    // taller box

  // cx, cy = center position of the box
  int16_t x = cx - BOX_W/2;
  int16_t y = cy - BOX_H/2;

  tft.fillRoundRect(x, y, BOX_W, BOX_H, 8, TFT_BLACK);
  tft.fillRoundRect(x+3, y+3, BOX_W-6, BOX_H-6, 8, color); // color = box color (green/yellow/red depending on pressure)

  tft.setTextColor(TFT_BLACK, color);
  tft.setTextSize(2);

  tft.setTextWrap(false);        // prevents wrapping
  tft.setTextDatum(MC_DATUM);    // center text

  tft.drawString(line1, cx, y + 16); // line1 = pressure string
  tft.drawString(line2, cx, y + 36); // line2 = temperature string
}

// draw the car image from panda.h
void drawBackground() {
  tft.fillScreen(TFT_BLACK); //clear screen to black

  TJpgDec.setJpgScale(1); //full res
  TJpgDec.setCallback(tjpg_callback);
  TJpgDec.setSwapBytes(true);

  // The Y offset (20) slightly moves the image down to center it
  TJpgDec.drawJpg(0, 20, img_asset, sizeof(img_asset)); 
}

void setup() {

  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);

  setupPositions();
  drawBackground();

  float testP[4] = {32.1,31.8,30.5,22.0}; // static test values (needs to be converted to get vals from xioas)
  float testT[4] = {36.2,35.9,34.8,31.0}; // static test values (needs to be converted to get vals from xioas)

    // Loop through the 4 tires
  for (int id = 0; id <= 3; id++) {

    char line1[20];
    char line2[20];
    
    snprintf(line1, sizeof(line1), "%.1f psi", testP[id]); // Format pressure text
    snprintf(line2, sizeof(line2), "%.1f C", testT[id]); // Format temperature text

    uint16_t color = TFT_GREEN; // Default box color

    // Change color based on pressure level
    if (testP[id] < 29) color = TFT_RED; // urgent low pressure warning
    else if (testP[id] < 31) color = TFT_YELLOW; // slightly low pressure

    drawBox(tire_info_pos[id].x, tire_info_pos[id].y, line1, line2, color); // Draw the tire information box
  }
}

void loop() {
  delay(1000);
}