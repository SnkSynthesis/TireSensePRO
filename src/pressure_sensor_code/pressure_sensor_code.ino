#include <Wire.h>

#define LED_PIN 2 

uint8_t id = 0x28;
uint8_t cmd[3] = {0xAA, 0x00, 0x00};
uint8_t data[7];
double pressure = 0;
double temperature = 0;
double percentage = 0;
double pressure_counts = 0;
double temp_counts = 0;

const double OUTPUTMAX = 15099494; // output at maximum pressure [counts]
const double OUTPUTMIN = 1677722; // output at minimum pressure [counts]
const double PMAX = 1; // maximum value of pressure range [bar, psi, kPa, etc.]
const double PMIN = 0; // minimum value of pressure range [bar, psi, kPa, etc.]
const double OFFSET = 2.2; // offset at gauge pressure 
char pBuff[20], percBuff[20], tBuff[20], printBuff[200]; 

void setup() {
  Serial.begin(9600);
  Wire.begin();
}

void loop() {
  Wire.beginTransmission(id);
  int stat = Wire.write(cmd, 3);
  stat |= Wire.endTransmission();
  delay(10);
  if (Wire.requestFrom(id, 7) != 7) {
    Serial.println("I2C read failed");
    return;
  } 
  for (int i = 0; i < 7; i++) {
    data[i] = Wire.read();
  }

  // raw I2C digital data
  pressure_counts = (data[1] << 16) | (data[2] << 8) | data[3];
  temp_counts = (data[4] << 16) | (data[5] << 8) | data[6];

  temperature = (temp_counts * 200 / 16777215) - 50; // calculate temperature in deg c
  percentage = (pressure_counts / 16777215) * 100; // calculate pressure as percentage of full scale
  //calculation of pressure value according to equation 2 of datasheet
  pressure = ((pressure_counts - OUTPUTMIN) * (PMAX - PMIN)) / (OUTPUTMAX - OUTPUTMIN) + PMIN;
  pressure *= 100;

  // if (pressure < 0) pressure = 0;

  dtostrf(pressure, 4, 1, pBuff);
  dtostrf(percentage, 4, 1, percBuff);
  dtostrf(temperature, 4, 3, tBuff);

  sprintf(printBuff, "%s PSI %\t %s %\t %s C \n", pBuff, percBuff, tBuff);
  Serial.println(printBuff);
  delay(10);
}