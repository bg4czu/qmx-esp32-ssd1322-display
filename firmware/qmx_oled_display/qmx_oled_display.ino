#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

constexpr uint8_t OLED_SCK = 12;
constexpr uint8_t OLED_MOSI = 11;
constexpr uint8_t OLED_RST = 13;
constexpr uint8_t OLED_DC = 9;
constexpr uint8_t OLED_CS = 10;
constexpr char DISPLAY_TITLE[] = "QMX+";
constexpr char SETUP_AP_SSID[] = "QMX_UTC";
constexpr char SETUP_AP_PASSWORD[] = "qmxutc88";  // Change before sharing hardware.
// Initial UART scan orientation. The firmware automatically swaps these pins
// until it receives valid QMX CAT replies; the verified wiring is documented
// in the project README.
constexpr uint8_t QMX_RX = 17;
constexpr uint8_t QMX_TX = 18;
constexpr uint32_t QMX_BAUD = 9600;

U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI oled(
    U8G2_R0, OLED_CS, OLED_DC, OLED_RST);
HardwareSerial qmx(1);
WebServer utcConfigServer(80);
DNSServer utcConfigDns;
Preferences utcPreferences;

String reply;
String usbCommand;
String pendingUtc;
uint64_t frequencyHz = 0;
uint64_t vfoAHz = 0;
uint64_t vfoBHz = 0;
char catMode = '?';
bool lowerSideband = false;
bool transmitting = false;
float powerWatts = 0.0f;
uint32_t lastPowerPeakMs = 0;
float swr = 0.0f;
float volumeDb = 0.0f;
bool volumeValid = false;
int signalDb = 0;
bool signalValid = false;
int filterWidthHz = 0;
int rfGainDb = 0;
int agcDb = 0;
int keyerWpm = 0;
bool swrProtection = false;
int ritHz = 0;
bool ritEnabled = false;
bool splitEnabled = false;
char activeVfo = 'A';
String cwDecoded;
char utcText[13] = "UTC --:--:--";
bool utcValid = false;
bool auxAlive = false;
bool pinsReversed = false;
uint32_t lastReplyMs = 0;
uint32_t lastQueryMs = 0;
uint32_t lastPinSwapMs = 0;
uint8_t queryStep = 0;

bool utcWifiConnecting = false;
bool utcWifiPortal = false;
bool ntpTimeValid = false;
bool ntpConfigured = false;
uint32_t utcWifiStartedMs = 0;
uint32_t utcPortalStartedMs = 0;
uint32_t lastNtpDisplayMs = 0;
uint32_t lastQmxNtpSetMs = 0;

int currentSolarFlux = 0;
int currentKIndex = -1;
bool solarDataValid = false;
volatile bool solarFetchRunning = false;
volatile bool solarUpdateReady = false;
uint32_t lastSolarFetchMs = 0;

void drawDisplay();

int solarXmlNumber(const String &body, const char *tag) {
  const String openTag = String("<") + tag + ">";
  const String closeTag = String("</") + tag + ">";
  int start = body.indexOf(openTag);
  if (start < 0) return -1;
  start += openTag.length();
  const int end = body.indexOf(closeTag, start);
  return end > start ? body.substring(start, end).toInt() : -1;
}

const char *bandPropagationText() {
  if (!solarDataValid || !frequencyHz) return "WAIT";
  if (currentKIndex >= 5) return "POOR";

  const float mhz = frequencyHz / 1000000.0f;
  int neededFlux = 70;
  if (mhz >= 50.0f) neededFlux = 170;
  else if (mhz >= 28.0f) neededFlux = 140;
  else if (mhz >= 24.0f) neededFlux = 120;
  else if (mhz >= 21.0f) neededFlux = 110;
  else if (mhz >= 18.0f) neededFlux = 100;
  else if (mhz >= 14.0f) neededFlux = 90;
  else if (mhz >= 10.0f) neededFlux = 80;

  if (currentKIndex <= 3 && currentSolarFlux >= neededFlux) return "GOOD";
  if (currentKIndex <= 4 && currentSolarFlux >= neededFlux - 25) return "FAIR";
  return "POOR";
}

void solarFetchTask(void *) {
  int newFlux = -1;
  int newK = -1;
  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient request;
  request.setTimeout(8000);
  if (request.begin(secure, "https://www.hamqsl.com/solarxml.php")) {
    if (request.GET() == HTTP_CODE_OK) {
      const String body = request.getString();
      newFlux = solarXmlNumber(body, "solarflux");
      newK = solarXmlNumber(body, "kindex");
    }
    request.end();
  }
  if (newFlux > 0 && newK >= 0) {
    currentSolarFlux = newFlux;
    currentKIndex = newK;
    solarDataValid = true;
  }
  lastSolarFetchMs = millis();
  solarUpdateReady = true;
  solarFetchRunning = false;
  vTaskDelete(nullptr);
}

void maintainSolarData() {
  if (solarUpdateReady) {
    solarUpdateReady = false;
    drawDisplay();
  }
  if (WiFi.status() != WL_CONNECTED || solarFetchRunning) return;
  // HAMQSL asks clients to update no more often than once per hour.
  if (lastSolarFetchMs != 0 && millis() - lastSolarFetchMs < 3600000UL) return;
  solarFetchRunning = true;
  if (xTaskCreate(solarFetchTask, "solar", 8192, nullptr, 1, nullptr) != pdPASS) {
    solarFetchRunning = false;
    lastSolarFetchMs = millis();
  }
}

void startUtcConfigPortal() {
  if (utcWifiPortal) return;
  utcWifiPortal = true;
  utcPortalStartedMs = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  delay(100);
  const IPAddress portalIp(192, 168, 77, 1);
  const IPAddress portalGateway(192, 168, 77, 1);
  const IPAddress portalMask(255, 255, 255, 0);
  WiFi.softAPConfig(portalIp, portalGateway, portalMask);
  const bool apStarted =
      WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD, 1, false, 4);
  utcConfigDns.start(53, "*", WiFi.softAPIP());

  utcConfigServer.on("/", HTTP_GET, []() {
    const char *page =
        "<!doctype html><meta name='viewport' content='width=device-width'>"
        "<meta charset='utf-8'><title>QMX UTC Wi-Fi</title>"
        "<style>body{font-family:sans-serif;max-width:420px;margin:35px auto;"
        "padding:15px}input,button{font-size:18px;width:100%;padding:10px;"
        "margin:7px 0;box-sizing:border-box}</style>"
        "<h2>QMX UTC Wi-Fi</h2><p>仅用于获取 UTC 时间（只支持 2.4 GHz）。</p>"
        "<form method='post' action='/save'><label>Wi-Fi 名称</label>"
        "<input name='s' required><label>Wi-Fi 密码</label>"
        "<input name='p' type='password'><button>保存并重启</button></form>";
    utcConfigServer.send(200, "text/html; charset=utf-8", page);
  });
  utcConfigServer.on("/save", HTTP_POST, []() {
    const String ssid = utcConfigServer.arg("s");
    const String password = utcConfigServer.arg("p");
    utcPreferences.begin("wifi", false);
    utcPreferences.putString("ssid", ssid);
    utcPreferences.putString("pass", password);
    utcPreferences.end();
    utcConfigServer.send(200, "text/html; charset=utf-8",
                         "<meta charset='utf-8'><h2>已保存，正在重启。</h2>");
    delay(800);
    ESP.restart();
  });
  utcConfigServer.onNotFound([]() {
    utcConfigServer.sendHeader("Location", "/", true);
    utcConfigServer.send(302, "text/plain", "");
  });
  utcConfigServer.begin();
  Serial.print("UTC setup AP QMX_UTC: ");
  Serial.println(apStarted ? "STARTED" : "FAILED");
  Serial.print("UTC setup IP: ");
  Serial.println(WiFi.softAPIP());
  snprintf(utcText, sizeof(utcText), "UTC WIFI SET");
  drawDisplay();
}

void startUtcWifi() {
  // Always provide a five-minute setup window, even if old credentials exist.
  startUtcConfigPortal();
  utcPreferences.begin("wifi", true);
  const String ssid = utcPreferences.getString("ssid", "");
  const String password = utcPreferences.getString("pass", "");
  utcPreferences.end();

  if (!ssid.length()) {
    return;
  }
  WiFi.begin(ssid.c_str(), password.c_str());
  utcWifiConnecting = true;
  utcWifiStartedMs = millis();
  snprintf(utcText, sizeof(utcText), "UTC WIFI...");
  Serial.print("Connecting UTC Wi-Fi: ");
  Serial.println(ssid);
}

void maintainUtcTime() {
  if (utcWifiPortal) {
    utcConfigDns.processNextRequest();
    utcConfigServer.handleClient();
    if (WiFi.status() == WL_CONNECTED &&
        millis() - utcPortalStartedMs >= 300000UL) {
      utcConfigServer.stop();
      utcConfigDns.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      utcWifiPortal = false;
      Serial.println("UTC setup AP closed after five minutes");
    }
  }

  if (utcWifiConnecting && WiFi.status() != WL_CONNECTED &&
      millis() - utcWifiStartedMs > 15000) {
    utcWifiConnecting = false;
    Serial.println("UTC Wi-Fi failed; starting setup AP");
    startUtcConfigPortal();
  }

  if (WiFi.status() != WL_CONNECTED) return;
  utcWifiConnecting = false;
  if (!ntpConfigured) {
    // Zero timezone and daylight offsets: the display and QMX are always UTC.
    configTime(0, 0, "ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org");
    ntpConfigured = true;
    snprintf(utcText, sizeof(utcText), "UTC NTP...");
    drawDisplay();
    Serial.print("UTC Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
  }

  if (millis() - lastNtpDisplayMs < 1000) return;
  lastNtpDisplayMs = millis();
  struct tm utc;
  if (!getLocalTime(&utc, 10) || utc.tm_year < 124) return;

  ntpTimeValid = true;
  snprintf(utcText, sizeof(utcText), "UTC %02d:%02d:%02d",
           utc.tm_hour, utc.tm_min, utc.tm_sec);
  utcValid = true;

  // Correct the QMX RTC after NTP lock, then refresh it every six hours.
  if (auxAlive && !transmitting &&
      (lastQmxNtpSetMs == 0 || millis() - lastQmxNtpSetMs >= 21600000UL)) {
    char command[12];
    snprintf(command, sizeof(command), "TM%02d%02d%02d;",
             utc.tm_hour, utc.tm_min, utc.tm_sec);
    qmx.print(command);
    lastQmxNtpSetMs = millis();
    Serial.print("QMX UTC synchronized by NTP: ");
    Serial.println(command);
  }
  drawDisplay();
}

void formatFrequencyValue(uint64_t value, char *out, size_t size) {
  if (!value) {
    snprintf(out, size, "--.---.--");
    return;
  }
  const uint32_t mhz = value / 1000000ULL;
  const uint32_t khz = (value / 1000ULL) % 1000ULL;
  const uint32_t tensHz = (value / 10ULL) % 100ULL;
  snprintf(out, size, "%lu.%03lu.%02lu", (unsigned long)mhz,
           (unsigned long)khz, (unsigned long)tensHz);
}

void formatFrequency(char *out, size_t size) {
  formatFrequencyValue(frequencyHz, out, size);
}

void formatBandwidth(char *out, size_t size) {
  if (filterWidthHz >= 1000) {
    snprintf(out, size, "%.1fk", filterWidthHz / 1000.0f);
  } else if (filterWidthHz > 0) {
    snprintf(out, size, "%dHz", filterWidthHz);
  } else {
    snprintf(out, size, "--Hz");
  }
}

const char *modeText() {
  static char unknown[5];
  switch (catMode) {
    case '0': return "IDLE";
    case '1': return "LSB";
    case '2': return "USB";
    case '3': return "CW";
    case '4': return "FM";
    case '5': return "AM";
    case '6': return "FSK";
    case '7': return "CW-R";
    case '8': return "TUNE";
    case '9': return "FSK-R";
    default:
      snprintf(unknown, sizeof(unknown), "M%c", catMode);
      return unknown;
  }
}

const char *bandText() {
  if (frequencyHz >= 1800000ULL && frequencyHz < 2000000ULL) return "160m";
  if (frequencyHz >= 3500000ULL && frequencyHz < 4000000ULL) return "80m";
  if (frequencyHz >= 5000000ULL && frequencyHz < 5500000ULL) return "60m";
  if (frequencyHz >= 7000000ULL && frequencyHz < 7300000ULL) return "40m";
  if (frequencyHz >= 10000000ULL && frequencyHz < 10200000ULL) return "30m";
  if (frequencyHz >= 14000000ULL && frequencyHz < 14350000ULL) return "20m";
  if (frequencyHz >= 18068000ULL && frequencyHz < 18168000ULL) return "17m";
  if (frequencyHz >= 21000000ULL && frequencyHz < 21450000ULL) return "15m";
  if (frequencyHz >= 24890000ULL && frequencyHz < 24990000ULL) return "12m";
  if (frequencyHz >= 28000000ULL && frequencyHz < 29700000ULL) return "10m";
  if (frequencyHz >= 50000000ULL && frequencyHz < 54000000ULL) return "6m";
  return "--m";
}

void drawCentered(const char *text, uint8_t y) {
  const int16_t width = oled.getStrWidth(text);
  const int16_t x = width < 256 ? (256 - width) / 2 : 0;
  oled.drawStr(x, y, text);
}

void drawDisplay() {
  char frequency[20];
  char volumeText[16];
  char powerText[20];
  char swrText[20];
  char statusText[20];
  char signalText[20];
  char bandwidthText[12];
  formatFrequency(frequency, sizeof(frequency));
  formatBandwidth(bandwidthText, sizeof(bandwidthText));
  const bool cwView = !transmitting && (catMode == '3' || catMode == '7');

  oled.clearBuffer();

  // Top status bar.
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(3, 9, DISPLAY_TITLE);
  if (utcWifiPortal && WiFi.status() != WL_CONNECTED) {
    snprintf(statusText, sizeof(statusText), "WIFI SETUP");
  } else if (!auxAlive) {
    snprintf(statusText, sizeof(statusText), "AUX SCANNING");
  } else if (swrProtection) {
    snprintf(statusText, sizeof(statusText), "SWR LOCK");
  } else if (splitEnabled) {
    snprintf(statusText, sizeof(statusText), "SPLIT VFO %c", activeVfo);
  } else if (ritEnabled) {
    snprintf(statusText, sizeof(statusText), "RIT %+d", ritHz);
  } else {
    snprintf(statusText, sizeof(statusText), "VFO %c", activeVfo);
  }
  const int16_t statusWidth = oled.getStrWidth(statusText);
  const int16_t statusX = 105 - statusWidth / 2;
  if (swrProtection && auxAlive) {
    oled.drawBox(statusX - 3, 0, statusWidth + 6, 11);
    oled.setDrawColor(0);
    oled.drawStr(statusX, 9, statusText);
    oled.setDrawColor(1);
  } else {
    oled.drawStr(statusX, 9, statusText);
  }
  oled.drawStr(146, 9, utcText);
  oled.drawBox(220, 0, 36, 11);
  oled.setDrawColor(0);
  oled.drawStr(transmitting ? 230 : 231, 9,
               auxAlive ? (transmitting ? "TX" : "RX") : "--");
  oled.setDrawColor(1);
  oled.drawHLine(0, 12, 256);

  // CW receive layout: compact frequency and two large decoded-text rows.
  if (cwView) {
    oled.setFont(u8g2_font_helvB12_tf);
    oled.drawStr(3, 29, frequency);
    const int16_t cwFrequencyWidth = oled.getStrWidth(frequency);
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(7 + cwFrequencyWidth, 29, "MHz");
    char cwInfo[28];
    snprintf(cwInfo, sizeof(cwInfo), "%s %dW %s", bandText(), keyerWpm,
             bandwidthText);
    const int16_t cwInfoWidth = oled.getStrWidth(cwInfo);
    oled.drawStr(254 - cwInfoWidth, 29, cwInfo);
    oled.drawHLine(0, 32, 256);

    String visible = cwDecoded.length() ? cwDecoded : "CW DECODER WAITING...";
    if (visible.length() > 56) visible = visible.substring(visible.length() - 56);
    while (visible.length() < 29) visible = " " + visible;
    String firstLine = visible.substring(0, min(28, (int)visible.length()));
    String secondLine = visible.length() > 28 ? visible.substring(28) : "";
    oled.setFont(u8g2_font_9x15_tf);
    oled.drawStr(2, 47, firstLine.c_str());
    oled.drawStr(2, 63, secondLine.c_str());
    oled.sendBuffer();
    return;
  }

  if (splitEnabled && vfoAHz && vfoBHz) {
    char rxFrequency[20];
    char txFrequency[20];
    formatFrequencyValue(vfoAHz, rxFrequency, sizeof(rxFrequency));
    formatFrequencyValue(vfoBHz, txFrequency, sizeof(txFrequency));
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(3, 26, "RX");
    oled.drawStr(3, 43, "TX");
    oled.setFont(u8g2_font_helvB12_tf);
    oled.drawStr(24, 27, rxFrequency);
    oled.drawStr(24, 44, txFrequency);
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(160, 26, "MHz");
    oled.drawStr(160, 43, "MHz");
    oled.drawFrame(205, 17, 51, 27);
    oled.setFont(u8g2_font_helvB10_tf);
    const char *band = bandText();
    const int16_t bandWidth = oled.getStrWidth(band);
    oled.drawStr(205 + (51 - bandWidth) / 2, 31, band);
    oled.setFont(u8g2_font_5x8_tf);
    const int16_t bwWidth = oled.getStrWidth(bandwidthText);
    oled.drawStr(205 + (51 - bwWidth) / 2, 42, bandwidthText);
  } else {
    // Large frequency with an automatic amateur-band and bandwidth badge.
    oled.setFont(u8g2_font_logisoso24_tn);
    oled.drawStr(3, 43, frequency);
    const int16_t frequencyWidth = oled.getStrWidth(frequency);
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(7 + frequencyWidth, 42, "MHz");
    oled.drawFrame(205, 17, 51, 27);
    oled.setFont(u8g2_font_helvB10_tf);
    const char *band = bandText();
    const int16_t bandWidth = oled.getStrWidth(band);
    oled.drawStr(205 + (51 - bandWidth) / 2, 31, band);
    oled.setFont(u8g2_font_5x8_tf);
    const int16_t bwWidth = oled.getStrWidth(bandwidthText);
    oled.drawStr(205 + (51 - bwWidth) / 2, 42, bandwidthText);
  }

  // Bottom bar changes automatically: decoded text on CW RX, meters otherwise.
  oled.drawHLine(0, 47, 256);
  oled.drawBox(0, 48, 50, 16);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_6x12_tf);
  const char *mode = modeText();
  const int16_t modeWidth = oled.getStrWidth(mode);
  const int16_t modeX = modeWidth < 48 ? (50 - modeWidth) / 2 : 1;
  oled.drawStr(modeX, 61, mode);
  oled.setDrawColor(1);
  oled.drawVLine(52, 49, 14);

  if (transmitting) {
    oled.drawVLine(106, 49, 14);
    oled.drawVLine(174, 49, 14);
    if (volumeValid) {
      snprintf(volumeText, sizeof(volumeText), "V %.1f", volumeDb);
    } else {
      snprintf(volumeText, sizeof(volumeText), "V --");
    }
    oled.drawStr(57, 61, volumeText);
    snprintf(powerText, sizeof(powerText),
             (catMode == '1' || catMode == '2') ? "PEP %.1fW" : "PWR %.1fW",
             powerWatts);
    oled.drawStr(112, 61, powerText);
    if (swr > 0.0f) {
      snprintf(swrText, sizeof(swrText), "SWR %.2f", swr);
    } else {
      snprintf(swrText, sizeof(swrText), "SWR --");
    }
    oled.drawStr(180, 61, swrText);
  } else {
    const uint8_t rxPage = (millis() / 3500UL) % 3;
    if (rxPage == 2) {
      // Current-band solar/geomagnetic hint. This is an index-based guide,
      // not a point-to-point propagation prediction.
      char propagation[20];
      char indices[20];
      snprintf(propagation, sizeof(propagation), "%s %s", bandText(),
               bandPropagationText());
      if (solarDataValid) {
        snprintf(indices, sizeof(indices), "SFI %d K%d", currentSolarFlux,
                 currentKIndex);
      } else {
        snprintf(indices, sizeof(indices), "SFI -- K-");
      }
      oled.drawVLine(112, 49, 14);
      oled.drawStr(57, 61, propagation);
      oled.drawStr(118, 61, indices);
      oled.sendBuffer();
      return;
    }

    oled.drawVLine(106, 49, 14);
    const bool showGain = rxPage == 1;
    if (showGain) {
      snprintf(volumeText, sizeof(volumeText), "RG %ddB", rfGainDb);
    } else if (volumeValid) {
      snprintf(volumeText, sizeof(volumeText), "V %.1f", volumeDb);
    } else {
      snprintf(volumeText, sizeof(volumeText), "V --");
    }
    oled.drawStr(57, 61, volumeText);
    if (showGain) {
      snprintf(signalText, sizeof(signalText), "AGC %ddB", agcDb);
    } else if (signalValid) {
      snprintf(signalText, sizeof(signalText), "S %ddB", signalDb);
    } else {
      snprintf(signalText, sizeof(signalText), "S --dB");
    }
    oled.drawStr(112, 61, signalText);
    oled.drawFrame(174, 52, 78, 9);
    if (signalValid) {
      // QMX SM is a positive meter value in dB; 0..90 gives useful headroom.
      int barWidth = map(constrain(signalDb, 0, 90), 0, 90, 0, 74);
      if (barWidth > 0) oled.drawBox(176, 54, barWidth, 5);
    }
  }
  oled.sendBuffer();
}

void handleReply(const String &s) {
  if (s.length() < 2) return;
  auxAlive = true;
  lastReplyMs = millis();

  if (s.startsWith("IF") && s.length() >= 30) {
    const bool wasTransmitting = transmitting;
    frequencyHz = strtoull(s.substring(2, 13).c_str(), nullptr, 10);
    ritHz = s.substring(18, 23).toInt();
    ritEnabled = s.length() > 23 && s.charAt(23) == '1';
    transmitting = s.charAt(28) == '1';
    if (transmitting && !wasTransmitting) {
      powerWatts = 0.0f;
      lastPowerPeakMs = 0;
    }
    catMode = s.charAt(29);
    if (s.length() > 30) activeVfo = s.charAt(30) == '1' ? 'B' : 'A';
    splitEnabled = s.length() > 32 && s.charAt(32) == '1';
    if (splitEnabled) {
      if (transmitting) vfoBHz = frequencyHz;
      else vfoAHz = frequencyHz;
    }
  } else if (s.startsWith("Q1") && s.length() >= 3) {
    lowerSideband = s.charAt(2) == '1';
  } else if (s.startsWith("PC") && s.length() > 2) {
    const float measuredWatts = s.substring(2).toInt() / 10.0f;
    if (transmitting && (catMode == '1' || catMode == '2')) {
      // SSB: capture voice peaks and hold PEP long enough to be readable.
      if (measuredWatts >= powerWatts || millis() - lastPowerPeakMs > 1200) {
        powerWatts = measuredWatts;
        lastPowerPeakMs = millis();
      }
    } else {
      powerWatts = measuredWatts;
      lastPowerPeakMs = millis();
    }
  } else if (s.startsWith("SW") && s.length() > 2) {
    swr = s.substring(2).toInt() / 100.0f;
  } else if (s.startsWith("AG") && s.length() > 2) {
    // QMX reports AF gain in quarter-dB steps (AG0056 = 14.0 dB).
    volumeDb = s.substring(2).toInt() / 4.0f;
    volumeValid = true;
  } else if (s.startsWith("SM") && s.length() > 2) {
    signalDb = s.substring(2).toInt();
    signalValid = true;
  } else if (s.startsWith("TB") && s.length() >= 5) {
    // TBtnns: t=TX-buffer state, nn=decoded character count, s=text.
    const int count = s.substring(3, 5).toInt();
    if (count > 0 && s.length() > 5) {
      cwDecoded += s.substring(5, min((int)s.length(), 5 + count));
      if (cwDecoded.length() > 120) {
        cwDecoded.remove(0, cwDecoded.length() - 120);
      }
    }
  } else if (s.startsWith("TM") && s.length() >= 8 && !ntpTimeValid) {
    snprintf(utcText, sizeof(utcText), "UTC %c%c:%c%c:%c%c",
             s.charAt(2), s.charAt(3), s.charAt(4), s.charAt(5),
             s.charAt(6), s.charAt(7));
    utcValid = true;
  } else if (s.startsWith("FW") && s.length() > 2) {
    filterWidthHz = s.substring(2).toInt();
  } else if (s.startsWith("RG") && s.length() > 2) {
    rfGainDb = s.substring(2).toInt();
  } else if (s.startsWith("SA") && s.length() > 2) {
    agcDb = s.substring(2).toInt();
  } else if (s.startsWith("SR") && s.length() > 2) {
    swrProtection = s.charAt(2) == '1';
  } else if (s.startsWith("KS") && s.length() > 2) {
    keyerWpm = s.substring(2).toInt();
  } else if (s.startsWith("FA") && s.length() > 2) {
    vfoAHz = strtoull(s.substring(2).c_str(), nullptr, 10);
  } else if (s.startsWith("FB") && s.length() > 2) {
    vfoBHz = strtoull(s.substring(2).c_str(), nullptr, 10);
  }

  Serial.print("QMX: ");
  Serial.println(s);
  drawDisplay();
}

void readQmx() {
  while (qmx.available()) {
    const char c = static_cast<char>(qmx.read());
    if (c == ';') {
      handleReply(reply);
      reply = "";
    } else if (c >= 32 && c <= 126) {
      if (reply.length() < 80) reply += c;
    }
  }
}

void readUsbCommand() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      usbCommand.trim();
      if (usbCommand.startsWith("SETUTC ") && usbCommand.length() == 13) {
        pendingUtc = usbCommand.substring(7);
        Serial.print("UTC set requested: ");
        Serial.println(pendingUtc);
      }
      usbCommand = "";
    } else if (c >= 32 && c <= 126 && usbCommand.length() < 32) {
      usbCommand += c;
    }
  }
}

void sendNextQuery() {
  static uint8_t txQueryStep = 0;
  static uint8_t cwQueryStep = 0;
  static bool sendIfNext = true;
  if (transmitting) {
    // Power is queried repeatedly so short SSB voice peaks are not missed.
    static const char *txQueries[] = {"PC;", "PC;", "IF;", "PC;", "SW;", "SR;"};
    qmx.print(txQueries[txQueryStep]);
    txQueryStep = (txQueryStep + 1) % 6;
    return;
  }
  if (catMode == '3' || catMode == '7') {
    // Prioritize the 40-character decoder buffer so CW text follows in real time.
    static const char *cwQueries[] = {"TB;", "IF;", "TB;", "SM;", "TB;", "IF;",
                                      "TB;", "TM;", "TB;", "KS;", "TB;", "FW;"};
    qmx.print(cwQueries[cwQueryStep]);
    cwQueryStep = (cwQueryStep + 1) % 12;
    return;
  }
  if (sendIfNext) {
    qmx.print("IF;");
    sendIfNext = false;
    return;
  }
  sendIfNext = true;
  if (splitEnabled) {
    static const char *splitQueries[] = {"FA;", "FB;", "TM;", "SM;", "AG;", "SR;", "FW;"};
    qmx.print(splitQueries[queryStep % 7]);
    queryStep = (queryStep + 1) % 7;
  } else {
    static const char *queries[] = {"AG;", "SM;", "TB;", "TM;", "Q1;", "PC;", "SW;",
                                    "FW;", "RG;", "SA;", "SR;", "KS;", "FA;", "FB;"};
    qmx.print(queries[queryStep]);
    queryStep = (queryStep + 1) % 14;
  }
}

void startQmxUart() {
  qmx.end();
  delay(10);
  if (pinsReversed) {
    qmx.begin(QMX_BAUD, SERIAL_8N1, QMX_TX, QMX_RX);
    Serial.println("Trying RX=GPIO18(red), TX=GPIO17(white)");
  } else {
    qmx.begin(QMX_BAUD, SERIAL_8N1, QMX_RX, QMX_TX);
    Serial.println("Trying RX=GPIO17(white), TX=GPIO18(red)");
  }
  reply = "";
  queryStep = 0;
  lastPinSwapMs = millis();
}

void setup() {
  Serial.begin(115200);
  SPI.begin(OLED_SCK, -1, OLED_MOSI, OLED_CS);
  oled.begin();
  oled.setContrast(180);
  startQmxUart();
  drawDisplay();
  startUtcWifi();
  Serial.println("QMX AUX display started (read-only CAT polling)");
}

void loop() {
  readQmx();
  readUsbCommand();
  maintainUtcTime();
  maintainSolarData();

  if (auxAlive && pendingUtc.length() == 6) {
    qmx.print("TM" + pendingUtc + ";");
    pendingUtc = "";
  }

  const bool cwReceive = !transmitting && (catMode == '3' || catMode == '7');
  const uint32_t queryInterval = transmitting ? 80 : (cwReceive ? 100 : 250);
  if (millis() - lastQueryMs >= queryInterval) {
    lastQueryMs = millis();
    sendNextQuery();
  }

  if (auxAlive && millis() - lastReplyMs > 2500) {
    auxAlive = false;
    drawDisplay();
  }

  if (!auxAlive && millis() - lastPinSwapMs >= 3000) {
    pinsReversed = !pinsReversed;
    startQmxUart();
  }

}
