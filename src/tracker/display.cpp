// OLED pages (map, peers, messages, GPS) and overlays (pairing code, SOS,
// banners, button hints, Morse draft). The map is drawn north-right by default:
// the board is held sideways with the antenna end pointing away from you.

#include "state.h"

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, 21, 18, 17); // reset, SCL, SDA

int page = PAGE_MAP;
String holdHint, draftLine;

static String banner;
static uint32_t bannerUntil = 0;
static uint32_t lastActivity = 0;
static bool asleep = false;

void displayBegin() {
  oled.begin();
  lastActivity = millis();
}

void displayWake() {
  lastActivity = millis();
  if (asleep) {
    asleep = false;
    oled.setPowerSave(0);
  }
}

bool displayAsleep() { return asleep; }

void showBanner(const String &text, uint32_t ms) {
  banner = text;
  bannerUntil = millis() + ms;
  displayWake();
}

static bool peerHasPos(const Peer &p) { return p.fix && gps.fix; }

// ---------------------------------------------------------------- pages

static void drawMapPage() {
  const int cx = 31, cy = 32, r = 31;
  uint32_t now = millis();

  // Ring radius: fit the farthest peer or waypoint, rounded up to a readable step
  static const double STEPS[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 1e6, 1e7};
  double farthest = 0;
  if (gps.fix) {
    for (int i = 0; i < numPeers; i++)
      if (peerHasPos(peers[i]) && now - peers[i].lastHeard < FORGET_MS)
        farthest = max(farthest, distM(gps.lat, gps.lon, peers[i].lat, peers[i].lon));
    for (int i = 0; i < numWaypoints; i++)
      farthest = max(farthest, distM(gps.lat, gps.lon, waypoints[i].lat, waypoints[i].lon));
  }
  double range = STEPS[0];
  for (double s : STEPS) {
    range = s;
    if (s >= farthest * 1.15) break;
  }

  // north/east metres -> pixels
  auto toScreen = [&](double n, double e, int &px, int &py) {
    double k = r / range;
    if (settings.northRight) {
      px = cx + lround(n * k);
      py = cy + lround(e * k);
    } else {
      px = cx + lround(e * k);
      py = cy - lround(n * k);
    }
    px = constrain(px, 1, 61);
    py = constrain(py, 1, 62);
  };

  oled.drawCircle(cx, cy, r);
  for (int a = 0; a < 360; a += 30) // dotted half-range ring
    oled.drawPixel(cx + lround(r / 2.0 * sin(radians(a))), cy - lround(r / 2.0 * cos(radians(a))));
  oled.setFont(u8g2_font_4x6_tf);
  if (settings.northRight) oled.drawStr(cx + r - 6, cy + 3, "N");
  else oled.drawStr(cx - 1, cy - r + 7, "N");

  if (gps.fix) {
    oled.drawDisc(cx, cy, 2);
    if (gps.speedKmh > 2) { // direction of travel: the best heading we have without a compass
      double n = cos(radians(gps.course)) * 10, e = sin(radians(gps.course)) * 10; // 10 px long
      int ex = settings.northRight ? cx + lround(n) : cx + lround(e);
      int ey = settings.northRight ? cy + lround(e) : cy - lround(n);
      oled.drawLine(cx, cy, ex, ey);
    }

    double mPerDegLat = 110540.0, mPerDegLon = 111320.0 * cos(radians(gps.lat));
    for (int i = 0; i < numWaypoints; i++) {
      int px, py;
      toScreen((waypoints[i].lat - gps.lat) * mPerDegLat, (waypoints[i].lon - gps.lon) * mPerDegLon, px, py);
      oled.drawLine(px - 2, py - 2, px + 2, py + 2);
      oled.drawLine(px - 2, py + 2, px + 2, py - 2);
    }
    for (int i = 0; i < numPeers; i++) {
      const Peer &p = peers[i];
      if (!peerHasPos(p) || now - p.lastHeard > FORGET_MS) continue;
      int px, py;
      toScreen((p.lat - gps.lat) * mPerDegLat, (p.lon - gps.lon) * mPerDegLon, px, py);
      if (now - p.lastHeard < STALE_MS) oled.drawBox(px - 1, py - 1, 3, 3);
      else oled.drawFrame(px - 1, py - 1, 3, 3);
      char label[3];
      snprintf(label, sizeof label, "%02X", p.id & 0xFF);
      oled.drawStr(px + 3 > 54 ? px - 10 : px + 3, py < 8 ? py + 7 : py + 2, label);
    }
  } else {
    oled.drawStr(cx - 11, cy + 3, "NO FIX");
  }

  // Right panel
  oled.setFont(u8g2_font_5x7_tf);
  oled.setCursor(66, 7);
  oled.printf("ME %04X", myId);
  oled.setCursor(66, 16);
  if (gps.fix) oled.printf("ring %s", fmtDist(range).c_str());
  else oled.printf("%d sat view", inViewTotal());

  int y = 27, shown = 0;
  for (int i = 0; i < numPeers && shown < 2; i++) {
    const Peer &p = peers[i];
    uint32_t age = now - p.lastHeard;
    if (age > FORGET_MS) continue;
    shown++;
    oled.setCursor(66, y);
    if (peerHasPos(p)) oled.printf("%04X %s", p.id, fmtDist(distM(gps.lat, gps.lon, p.lat, p.lon)).c_str());
    else oled.printf("%04X %s", p.id, p.fix ? "?" : "nofix");
    oled.setCursor(66, y + 8);
    if (age > STALE_MS) oled.printf(" %s ago", fmtAge(age).c_str());
    else if (peerHasPos(p)) {
      double b = bearingDeg(gps.lat, gps.lon, p.lat, p.lon);
      oled.printf("%-2s%3d %ddB", compass(b), (int)lround(b) % 360, (int)p.rssi);
    } else oled.printf(" %ddB", (int)p.rssi);
    y += 19;
  }
  if (!shown) oled.drawStr(66, y, "nobody yet");
}

static void drawPeersPage() {
  uint32_t now = millis();
  oled.setFont(u8g2_font_5x7_tf);
  oled.setCursor(0, 7);
  oled.printf("PEERS %d   me %04X", numPeers, myId);
  oled.drawHLine(0, 9, 128);
  int y = 18;
  for (int i = 0; i < numPeers && y <= 64; i++, y += 9) {
    const Peer &p = peers[i];
    String where = peerHasPos(p) ? fmtDist(distM(gps.lat, gps.lon, p.lat, p.lon)) + " " +
                                       compass(bearingDeg(gps.lat, gps.lon, p.lat, p.lon))
                                 : (p.fix ? "you:nofix" : "no fix");
    String bat = p.bat < 0 ? "--" : String(p.bat) + "%";
    oled.setCursor(0, y);
    oled.printf("%04X %-9s %4s %s", p.id, where.c_str(), bat.c_str(), fmtAge(now - p.lastHeard).c_str());
  }
  if (!numPeers) oled.drawStr(0, 30, "Nobody heard yet");
}

static void drawCheck(int x, int y) { // 5x4 tick, baseline y
  oled.drawLine(x, y - 2, x + 1, y - 1);
  oled.drawLine(x + 1, y - 1, x + 4, y - 4);
}

static void drawMsgsPage() {
  uint32_t now = millis();
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(0, 7, "MSGS  tap=. press=-");
  oled.drawHLine(0, 9, 128);
  for (int i = 0; i < min(numMsgs, 6); i++) {
    const Message &m = msgs[i];
    int y = 18 + i * 9;
    oled.setCursor(0, y);
    if (m.from == myId) oled.printf("me  : %s", m.text.c_str());
    else oled.printf("%04X: %s", m.from, m.text.c_str());
    String age = fmtAge(now - m.at);
    int ax = 128 - oled.getStrWidth(age.c_str());
    oled.drawStr(ax, y, age.c_str());
    if (m.from == myId) {
      if (m.state == MSG_OK) drawCheck(ax - 7, y);
      else if (m.state == MSG_FAIL) oled.drawStr(ax - 6, y, "!");
      else oled.drawStr(ax - 7, y, "~");
    }
  }
  if (!numMsgs) oled.drawStr(0, 30, "No messages yet");
}

static void drawGpsPage() {
  oled.setFont(u8g2_font_6x10_tf);
  oled.setCursor(0, 10);
  if (millis() - gps.lastSentence > 3000) oled.print("GPS SILENT (GPIO41)");
  else if (gps.fix) oled.printf("FIX  used %d/%d sats", gps.sats, inViewTotal());
  else oled.printf("NO FIX  %d in view", inViewTotal());
  oled.setCursor(0, 21);
  oled.printf("GP%d BD%d GL%d GA%d", gps.inView[0], gps.inView[1], gps.inView[2], gps.inView[3]);
  if (gps.fix) {
    oled.setCursor(0, 32);
    oled.printf("%.6f", gps.lat);
    oled.setCursor(0, 43);
    oled.printf("%.6f", gps.lon);
    oled.setCursor(0, 54);
    oled.printf("%.0fm %.1fkm/h hd%.1f", gps.alt, gps.speedKmh, gps.hdop);
  }
  oled.setCursor(0, 64);
  const String &t = gps.time;
  if (t.length() >= 6) oled.printf("%s:%s:%s UTC", t.substring(0, 2).c_str(), t.substring(2, 4).c_str(), t.substring(4, 6).c_str());
  else oled.print("--:--:-- UTC");
  if (batteryPct >= 0) {
    String b = String(batteryPct) + "%";
    oled.drawStr(128 - oled.getStrWidth(b.c_str()), 64, b.c_str());
  }
}

// ---------------------------------------------------------------- overlays

static void drawBtIcon() {
  if (!bleIsOn()) return;
  const int x = 122, y = 0;
  bool conn = bleConnected();
  if (conn) {
    oled.drawBox(x - 1, y, 7, 10);
    oled.setDrawColor(0);
  }
  oled.drawVLine(x + 2, y + 1, 8);
  oled.drawLine(x + 2, y + 1, x + 4, y + 3);
  oled.drawLine(x + 4, y + 3, x, y + 7);
  oled.drawLine(x + 2, y + 8, x + 4, y + 6);
  oled.drawLine(x + 4, y + 6, x, y + 2);
  oled.setDrawColor(1);
}

static bool drawPairing() {
  uint32_t code;
  if (!blePairing(&code)) return false;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(4, 12, "Bluetooth pairing");
  oled.drawStr(4, 24, "Enter on phone:");
  oled.setFont(u8g2_font_logisoso22_tn);
  char b[8];
  snprintf(b, sizeof b, "%06lu", (unsigned long)code);
  oled.drawStr((128 - oled.getStrWidth(b)) / 2, 58, b);
  return true;
}

static void drawSos() {
  uint16_t from;
  if (!sosAlarm(&from)) return;
  bool flash = (millis() / 400) % 2;
  oled.setDrawColor(flash ? 1 : 0);
  oled.drawBox(0, 0, 128, 30);
  oled.setDrawColor(flash ? 0 : 1);
  oled.drawFrame(0, 0, 128, 30);
  oled.setFont(u8g2_font_6x10_tf);
  oled.setCursor(3, 11);
  oled.printf("!! SOS from %04X !!", from);
  oled.setCursor(3, 25);
  Peer *p = findPeer(from);
  if (p && p->fix && gps.fix) {
    double b = bearingDeg(gps.lat, gps.lon, p->lat, p->lon);
    oled.printf("%s %s  press=ok", fmtDist(distM(gps.lat, gps.lon, p->lat, p->lon)).c_str(), compass(b));
  } else oled.print("press button = ok");
  oled.setDrawColor(1);
}

static void drawBanner() {
  if ((int32_t)(millis() - bannerUntil) >= 0) return;
  oled.setFont(u8g2_font_6x10_tf);
  oled.setDrawColor(0);
  oled.drawBox(0, 0, 128, 12);
  oled.setDrawColor(1);
  oled.drawFrame(0, 0, 128, 12);
  oled.drawStr(2, 10, banner.c_str());
}

static void drawBottomBar() {
  const String &text = holdHint.length() ? holdHint : draftLine;
  if (!text.length()) return;
  oled.setFont(u8g2_font_6x10_tf);
  String s = text;
  while (oled.getStrWidth(s.c_str()) > 124 && s.length() > 3) s.remove(2, 1);
  oled.drawBox(0, 53, 128, 11);
  oled.setDrawColor(0);
  oled.drawStr(2, 62, s.c_str());
  oled.setDrawColor(1);
}

void displayUpdate() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  if (now - lastDraw < 150) return;
  lastDraw = now;

  bool urgent = sosAlarm(nullptr) || blePairing(nullptr) || holdHint.length() || draftLine.length();
  if (urgent) displayWake();
  if (!asleep && settings.screenS && now - lastActivity > settings.screenS * 1000UL) {
    asleep = true;
    oled.setPowerSave(1);
  }
  if (asleep) return;

  if (drawPairing()) {
    oled.sendBuffer();
    return;
  }
  oled.clearBuffer();
  switch (page) {
    case PAGE_MAP: drawMapPage(); break;
    case PAGE_PEERS: drawPeersPage(); break;
    case PAGE_MSGS: drawMsgsPage(); break;
    case PAGE_GPS: drawGpsPage(); break;
  }
  drawBtIcon();
  drawBanner();
  drawSos();
  drawBottomBar();
  oled.sendBuffer();
}
