#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// Gain must be GAIN_TWOTHIRDS (±6.144V) because the INFKL-15 outputs up to 4.9V at 150°C. Lower gain settings would clip
// Using computeVolts() for  correct scaling for selected gain
Adafruit_ADS1115 ads;

struct sensor {
  float volts[4];
  float tempC[4];
};

void setup() {
  Serial.begin(9600);
  Serial.println("Hello World");
  ads.begin();

  ads.setGain(GAIN_TWOTHIRDS);
}

sensor s0;

void loop() {

  for (int i = 0; i < 4; i++) {
    int16_t raw = ads.readADC_SingleEnded(i);

    s0.volts[i] = ads.computeVolts(raw);

    // transfer function (30mV/C, 0.40V offset)
    s0.tempC[i] = (s0.volts[i] - 0.40) / 0.030;
  }

  Serial.printf(
    "A0: %.2f C | A1: %.2f C | A2: %.2f C | A3: %.2f C\n",
    s0.tempC[0],
    s0.tempC[1],
    s0.tempC[2],
    s0.tempC[3]
  );

  delay(1000);x 
}
