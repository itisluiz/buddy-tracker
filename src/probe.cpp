// GPS probe for Heltec WiFi LoRa 32 V3.
// Listens for NMEA on GPIO41 and GPIO42 simultaneously (RX-only on each),
// auto-detecting the baud rate. Whichever pin receives valid NMEA is wired to
// the GPS module's TX, so that pin is the ESP32's RX.

#include <Arduino.h>
#include <U8g2lib.h>

static const int VEXT_PIN = 36; // LOW = Vext rail (Ve pin + OLED) powered

// Onboard 128x64 SSD1306: reset=21, SCL=18, SDA=17
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, 21, 18, 17);
static const uint32_t BAUDS[] = {9600, 38400, 115200, 57600, 4800, 19200, 230400};
static const size_t NUM_BAUDS = sizeof(BAUDS) / sizeof(BAUDS[0]);
static const uint32_t BAUD_DWELL_MS = 2500;

struct Probe {
  HardwareSerial *uart;
  int pin;
  size_t baudIdx = 0;
  bool locked = false;
  uint32_t lastSwitch = 0;
  uint32_t bytes = 0;     // bytes since last baud switch
  uint32_t totalBytes = 0;
  uint32_t goodSentences = 0;
  uint32_t badSentences = 0;
  String line;

  // Latest parsed fix data
  String fixTime, lat, lon, alt;
  int fixQuality = -1, sats = -1;
  char rmcStatus = '?';
  String antenna;
  int inView[4] = {0, 0, 0, 0}; // GP, BD/GB, GL, GA
};

static int talkerIdx(const String &s) {
  String t = s.substring(1, 3);
  if (t == "GP") return 0;
  if (t == "BD" || t == "GB") return 1;
  if (t == "GL") return 2;
  if (t == "GA") return 3;
  return -1;
}

static Probe probes[2];

static void startUart(Probe &p) {
  p.uart->end();
  p.uart->begin(BAUDS[p.baudIdx], SERIAL_8N1, p.pin, -1);
  p.lastSwitch = millis();
  p.bytes = 0;
  p.line = "";
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool checksumOk(const String &s) {
  int star = s.lastIndexOf('*');
  if (s.length() < 6 || s[0] != '$' || star < 0 || star + 2 >= (int)s.length()) return false;
  uint8_t sum = 0;
  for (int i = 1; i < star; i++) sum ^= (uint8_t)s[i];
  int hi = hexVal(s[star + 1]), lo = hexVal(s[star + 2]);
  return hi >= 0 && lo >= 0 && sum == ((hi << 4) | lo);
}

static String field(const String &s, int n) {
  int start = 0;
  for (int i = 0; i < n; i++) {
    start = s.indexOf(',', start);
    if (start < 0) return "";
    start++;
  }
  int end = s.indexOf(',', start);
  int star = s.indexOf('*', start);
  if (end < 0 || (star >= 0 && star < end)) end = star;
  return end < 0 ? s.substring(start) : s.substring(start, end);
}

static String fmtCoord(const String &v, const String &hemi, int degDigits) {
  if (v.length() < (unsigned)degDigits + 2) return "";
  double deg = v.substring(0, degDigits).toDouble();
  double min = v.substring(degDigits).toDouble();
  double d = deg + min / 60.0;
  if (hemi == "S" || hemi == "W") d = -d;
  return String(d, 6);
}

static void handleSentence(Probe &p, const String &s) {
  if (!checksumOk(s)) {
    p.badSentences++;
    return;
  }
  p.goodSentences++;
  if (!p.locked && p.goodSentences >= 2) {
    p.locked = true;
    Serial.printf("\n>>> GPS DETECTED on GPIO%d at %lu baud  ->  GPS TX is wired to GPIO%d (use it as ESP RX)\n\n",
                  p.pin, (unsigned long)BAUDS[p.baudIdx], p.pin);
  }
  Serial.printf("[GPIO%d] %s\n", p.pin, s.c_str());

  String type = s.substring(3, 6);
  if (type == "GGA") {
    p.fixTime = field(s, 1);
    p.fixQuality = field(s, 6).toInt();
    p.sats = field(s, 7).toInt();
    p.lat = fmtCoord(field(s, 2), field(s, 3), 2);
    p.lon = fmtCoord(field(s, 4), field(s, 5), 3);
    p.alt = field(s, 9);
  } else if (type == "RMC") {
    String st = field(s, 2);
    p.rmcStatus = st.length() ? st[0] : '?';
  } else if (type == "GSV") {
    int idx = talkerIdx(s);
    if (idx >= 0) p.inView[idx] = field(s, 3).toInt();
  } else if (type == "TXT") {
    String msg = field(s, 4);
    if (msg.startsWith("ANTENNA")) p.antenna = msg.substring(8);
  }
}

static void pollProbe(Probe &p) {
  while (p.uart->available()) {
    char c = (char)p.uart->read();
    p.bytes++;
    p.totalBytes++;
    if (c == '$') p.line = "$";
    else if (c == '\r' || c == '\n') {
      if (p.line.length() > 1) handleSentence(p, p.line);
      p.line = "";
    } else if (p.line.length() && p.line.length() < 120) p.line += c;
  }

  if (!p.locked && millis() - p.lastSwitch > BAUD_DWELL_MS) {
    p.badSentences = 0;
    p.goodSentences = 0;
    p.baudIdx = (p.baudIdx + 1) % NUM_BAUDS;
    startUart(p);
  }
}

static void printStatus() {
  Serial.println("---------------- status ----------------");
  for (auto &p : probes) {
    if (p.locked) {
      const char *q = p.fixQuality <= 0 ? "NO FIX (searching)" : (p.fixQuality == 1 ? "GPS fix" : "DGPS/other fix");
      Serial.printf("GPIO%d: GPS @ %lu baud | %s | sats=%d | RMC=%c | time=%s UTC\n",
                    p.pin, (unsigned long)BAUDS[p.baudIdx], q, p.sats, p.rmcStatus, p.fixTime.c_str());
      if (p.fixQuality > 0)
        Serial.printf("        lat=%s lon=%s alt=%sm  https://maps.google.com/?q=%s,%s\n",
                      p.lat.c_str(), p.lon.c_str(), p.alt.c_str(), p.lat.c_str(), p.lon.c_str());
    } else {
      Serial.printf("GPIO%d: scanning, trying %lu baud | bytes seen total=%lu (%s)\n",
                    p.pin, (unsigned long)BAUDS[p.baudIdx], (unsigned long)p.totalBytes,
                    p.totalBytes ? "line is active, finding baud" : "silent");
    }
  }
  Serial.println("----------------------------------------");
}

static void drawDisplay() {
  Probe *gps = nullptr;
  for (auto &p : probes)
    if (p.locked) gps = &p;

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  if (!gps) {
    oled.drawStr(0, 10, "GPS probe: scanning");
    int y = 26;
    for (auto &p : probes) {
      oled.setCursor(0, y);
      oled.printf("GPIO%d %lu %s", p.pin, (unsigned long)BAUDS[p.baudIdx], p.totalBytes ? "active" : "silent");
      y += 12;
    }
    oled.sendBuffer();
    return;
  }

  bool fix = gps->fixQuality > 0;
  int inView = gps->inView[0] + gps->inView[1] + gps->inView[2] + gps->inView[3];
  oled.setCursor(0, 10);
  oled.printf("RX=GPIO%d %lubd", gps->pin, (unsigned long)BAUDS[gps->baudIdx]);
  oled.setCursor(0, 21);
  if (fix) oled.printf("FIX  used %d/%d sats", gps->sats, inView);
  else oled.printf("NO FIX  %d in view", inView);
  if (fix) {
    oled.setCursor(0, 32);
    oled.printf("Lat %s", gps->lat.c_str());
    oled.setCursor(0, 43);
    oled.printf("Lon %s", gps->lon.c_str());
    oled.setCursor(0, 54);
    oled.printf("Alt %sm", gps->alt.c_str());
  } else {
    oled.setCursor(0, 32);
    oled.printf("GP%d BD%d GL%d GA%d", gps->inView[0], gps->inView[1], gps->inView[2], gps->inView[3]);
    oled.setCursor(0, 43);
    oled.print("Waiting for sky...");
  }
  oled.setCursor(0, 64);
  const String &t = gps->fixTime;
  if (t.length() >= 6) oled.printf("%s:%s:%s UTC", t.substring(0, 2).c_str(), t.substring(2, 4).c_str(), t.substring(4, 6).c_str());
  else oled.print("--:--:-- UTC");
  if (gps->antenna.length()) {
    String a = "Ant " + gps->antenna;
    oled.drawStr(128 - oled.getStrWidth(a.c_str()), 64, a.c_str());
  }
  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW); // power GPS if it hangs off the Ve pin
  delay(500);
  Serial.println("\nHeltec V3 GPS probe: listening on GPIO41 and GPIO42");
  oled.begin();

  probes[0].uart = &Serial1;
  probes[0].pin = 41;
  probes[1].uart = &Serial2;
  probes[1].pin = 42;
  for (auto &p : probes) startUart(p);
}

void loop() {
  static uint32_t lastStatus = 0, lastDraw = 0;
  for (auto &p : probes) pollProbe(p);
  if (millis() - lastDraw > 500) {
    lastDraw = millis();
    drawDisplay();
  }
  if (millis() - lastStatus > 3000) {
    lastStatus = millis();
    printStatus();
  }
}
