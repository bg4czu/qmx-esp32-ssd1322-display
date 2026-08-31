#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

constexpr uint8_t OLED_SCK = 12;
constexpr uint8_t OLED_MOSI = 11;
constexpr uint8_t OLED_RST = 13;
constexpr uint8_t OLED_DC = 9;
constexpr uint8_t OLED_CS = 10;
constexpr uint8_t PROBE_17 = 17;  // Currently white wire
constexpr uint8_t PROBE_18 = 18;  // Currently red wire

U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI oled(
    U8G2_R0, OLED_CS, OLED_DC, OLED_RST);

uint32_t averageMillivolts(uint8_t pin) {
  uint32_t total = 0;
  for (int i = 0; i < 32; ++i) {
    total += analogReadMilliVolts(pin);
    delayMicroseconds(200);
  }
  return total / 32;
}

void drawReadings(uint32_t mv17, uint32_t mv18) {
  char line[32];
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(4, 12, "PASSIVE AUX VOLTAGE PROBE");
  oled.drawStr(4, 25, "NO CAT DATA IS TRANSMITTED");

  oled.setFont(u8g2_font_helvB14_tf);
  snprintf(line, sizeof(line), "17/W: %.2fV", mv17 / 1000.0f);
  oled.drawStr(4, 45, line);
  snprintf(line, sizeof(line), "18/R: %.2fV", mv18 / 1000.0f);
  oled.drawStr(132, 45, line);

  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(4, 62, "BLACK WIRE MUST BE GND/SLEEVE");
  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  pinMode(PROBE_17, INPUT);
  pinMode(PROBE_18, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PROBE_17, ADC_11db);
  analogSetPinAttenuation(PROBE_18, ADC_11db);

  SPI.begin(OLED_SCK, -1, OLED_MOSI, OLED_CS);
  oled.begin();
  oled.setContrast(180);
  Serial.println("Passive AUX voltage probe started");
}

void loop() {
  const uint32_t mv17 = averageMillivolts(PROBE_17);
  const uint32_t mv18 = averageMillivolts(PROBE_18);
  drawReadings(mv17, mv18);
  Serial.printf("GPIO17/white=%lumV GPIO18/red=%lumV\n",
                static_cast<unsigned long>(mv17),
                static_cast<unsigned long>(mv18));
  delay(500);
}
