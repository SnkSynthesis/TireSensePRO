#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "car_image.h"

// ==== CHANGE THESE TO MATCH YOUR BOARD ====
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
// ==========================================

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

#define SCREEN_W 320
#define SCREEN_H 240

float pressure[4];
float temperature[4];

float prevPressure[4];
float prevTemp[4];

struct TirePos {
    int x;
    int y;
};

TirePos tirePos[4] = {
    {20, 60},    // FL
    {240, 60},   // FR
    {20, 160},   // RL
    {240, 160}   // RR
};

uint16_t pressureColor(float psi) {
    if (psi < 28) return ILI9341_RED;
    if (psi < 32) return ILI9341_YELLOW;
    return ILI9341_GREEN;
}

uint16_t tempColor(float temp) {
    if (temp > 90) return ILI9341_RED;
    if (temp > 75) return ILI9341_YELLOW;
    return ILI9341_WHITE;
}

void drawStaticUI() {

    tft.fillScreen(ILI9341_BLACK);

    tft.setTextSize(1);
    tft.setTextColor(ILI9341_DARKGREY);

    tft.setCursor(20, 40);
    tft.print("FRONT LEFT");

    tft.setCursor(220, 40);
    tft.print("FRONT RIGHT");

    tft.setCursor(20, 140);
    tft.print("REAR LEFT");

    tft.setCursor(220, 140);
    tft.print("REAR RIGHT");

    // Draw car image centered
    int imgW = 120;
    int imgH = 200;
    int x = (SCREEN_W - imgW) / 2;
    int y = (SCREEN_H - imgH) / 2;

    tft.drawRGBBitmap(x, y, carImage, imgW, imgH);
}

void updateTire(int i) {

    if (pressure[i] == prevPressure[i] &&
        temperature[i] == prevTemp[i]) return;

    int x = tirePos[i].x;
    int y = tirePos[i].y;

    // Clear only number area
    tft.fillRect(x, y, 90, 50, ILI9341_BLACK);

    tft.setTextSize(2);

    tft.setTextColor(pressureColor(pressure[i]));
    tft.setCursor(x, y);
    tft.print("P: ");
    tft.print(pressure[i], 1);

    tft.setTextColor(tempColor(temperature[i]));
    tft.setCursor(x, y + 22);
    tft.print("T: ");
    tft.print(temperature[i], 1);
    tft.print("C");

    prevPressure[i] = pressure[i];
    prevTemp[i] = temperature[i];
}

void simulateData() {

    for (int i = 0; i < 4; i++) {

        pressure[i] += random(-5, 6) * 0.02;
        temperature[i] += random(-5, 6) * 0.05;

        pressure[i] = constrain(pressure[i], 26, 36);
        temperature[i] = constrain(temperature[i], 20, 95);
    }
}

void setup() {

    tft.begin();
    tft.setRotation(1);

    randomSeed(analogRead(0));

    for (int i = 0; i < 4; i++) {
        pressure[i] = 32.0;
        temperature[i] = 45.0;
        prevPressure[i] = -1;
        prevTemp[i] = -1;
    }

    drawStaticUI();
}

void loop() {

    simulateData();

    for (int i = 0; i < 4; i++) {
        updateTire(i);
    }

    delay(200);
}