#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

constexpr uint8_t OLED_SCK = 12;
constexpr uint8_t OLED_MOSI = 11;
constexpr uint8_t OLED_RST = 13;
constexpr uint8_t OLED_DC = 9;
constexpr uint8_t OLED_CS = 10;
constexpr uint8_t WHITE_PIN = 17;
constexpr uint8_t RED_PIN = 18;

U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI oled(
    U8G2_R0, OLED_CS, OLED_DC, OLED_RST);

int lastWhite = -1;
int lastRed = -1;
uint32_t lastPrintMs = 0;

void drawState(int white, int red) {
  char line[32];
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(4, 12, "3.5mm CABLE CONTINUITY");
  oled.drawStr(4, 25, "BLACK WIRE = GND REFERENCE");

  oled.setFont(u8g2_font_helvB14_tf);
  snprintf(line, sizeof(line), "17 WHITE: %s", white ? "HIGH" : "LOW");
  oled.drawStr(4, 45, line);
  snprintf(line, sizeof(line), "18 RED: %s", red ? "HIGH" : "LOW");
  oled.drawStr(132, 45, line);

  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(4, 62, "SHORT PLUG SECTIONS ONLY");
  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  pinMode(WHITE_PIN, INPUT_PULLUP);
  pinMode(RED_PIN, INPUT_PULLUP);
  SPI.begin(OLED_SCK, -1, OLED_MOSI, OLED_CS);
  oled.begin();
  oled.setContrast(180);
  Serial.println("Cable continuity tester started");
}

void loop() {
  const int white = digitalRead(WHITE_PIN);
  const int red = digitalRead(RED_PIN);
  if (white != lastWhite || red != lastRed) {
    lastWhite = white;
    lastRed = red;
    drawState(white, red);
  }
  if (millis() - lastPrintMs >= 500) {
    lastPrintMs = millis();
    Serial.printf("WHITE_GPIO17=%s RED_GPIO18=%s\n",
                  white ? "HIGH" : "LOW", red ? "HIGH" : "LOW");
  }
  delay(20);
}
