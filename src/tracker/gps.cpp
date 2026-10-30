// ATGM336H NMEA parsing: fix, time/date, HDOP, and per-satellite sky data.

#include "state.h"

static const uint32_t GPS_BAUD = 9600;
static const int SYS_SATS = 16;

static String line;

// Satellites per system: GSV cycles are collected in `building` and swapped
// into `committed` once the cycle's last sentence arrives.
static Sat building[4][SYS_SATS], committed[4][SYS_SATS];
static int numBuilding[4], numCommitted[4];
static uint8_t usedPrn[4][12];
static int numUsed[4];

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

static int fieldCount(const String &s) {
  int star = s.indexOf('*');
  int n = 1;
  for (int i = 0; i < (star < 0 ? (int)s.length() : star); i++)
    if (s[i] == ',') n++;
  return n;
}

static double parseCoord(const String &v, const String &hemi, int degDigits) {
  if (v.length() < (unsigned)degDigits + 2) return NAN;
  double d = v.substring(0, degDigits).toDouble() + v.substring(degDigits).toDouble() / 60.0;
  return (hemi == "S" || hemi == "W") ? -d : d;
}

static int talkerSys(const String &s) {
  String t = s.substring(1, 3);
  if (t == "GP") return 0;
  if (t == "BD" || t == "GB") return 1;
  if (t == "GL") return 2;
  if (t == "GA") return 3;
  return -1;
}

static void rebuildSky() {
  gps.numSky = 0;
  for (int sys = 0; sys < 4; sys++)
    for (int i = 0; i < numCommitted[sys] && gps.numSky < MAX_SKY; i++) {
      Sat s = committed[sys][i];
      s.used = false;
      for (int u = 0; u < numUsed[sys]; u++)
        if (usedPrn[sys][u] == s.prn) s.used = true;
      gps.sky[gps.numSky++] = s;
    }
}

static void handleGsv(const String &s) {
  int sys = talkerSys(s);
  if (sys < 0) return;
  int total = field(s, 1).toInt(), num = field(s, 2).toInt();
  gps.inView[sys] = field(s, 3).toInt();
  if (num == 1) numBuilding[sys] = 0;
  int groups = (fieldCount(s) - 4) / 4;
  for (int g = 0; g < groups && numBuilding[sys] < SYS_SATS; g++) {
    String prn = field(s, 4 + g * 4);
    if (!prn.length()) continue;
    Sat &sat = building[sys][numBuilding[sys]++];
    sat.sys = sys;
    sat.prn = prn.toInt();
    sat.el = field(s, 5 + g * 4).toInt();
    sat.az = field(s, 6 + g * 4).toInt();
    sat.snr = field(s, 7 + g * 4).toInt();
  }
  if (num == total) {
    memcpy(committed[sys], building[sys], sizeof(Sat) * numBuilding[sys]);
    numCommitted[sys] = numBuilding[sys];
    rebuildSky();
  }
}

static void handleGsa(const String &s) {
  // NMEA 4.1 puts the system ID in field 18 (1 GPS, 2 GLONASS, 3 Galileo, 4 BeiDou)
  static const int SYS_FROM_ID[] = {-1, 0, 2, 3, 1};
  String sysField = field(s, 18);
  int sys = sysField.length() ? (sysField.toInt() >= 1 && sysField.toInt() <= 4 ? SYS_FROM_ID[sysField.toInt()] : -1)
                              : talkerSys(s);
  if (sys < 0) return;
  numUsed[sys] = 0;
  for (int i = 3; i <= 14; i++) {
    String prn = field(s, i);
    if (prn.length()) usedPrn[sys][numUsed[sys]++] = prn.toInt();
  }
  rebuildSky();
}

static void handleSentence(const String &s) {
  if (!checksumOk(s)) return;
  gps.lastSentence = millis();
  String type = s.substring(3, 6);
  if (type == "GGA") {
    gps.time = field(s, 1);
    gps.sats = field(s, 7).toInt();
    gps.lat = parseCoord(field(s, 2), field(s, 3), 2);
    gps.lon = parseCoord(field(s, 4), field(s, 5), 3);
    String hdop = field(s, 8);
    gps.hdop = hdop.length() ? hdop.toFloat() : 99;
    gps.alt = field(s, 9).toFloat();
    gps.fix = field(s, 6).toInt() > 0 && !isnan(gps.lat) && !isnan(gps.lon);
  } else if (type == "RMC") {
    gps.speedKmh = field(s, 7).toFloat() * 1.852f;
    gps.course = field(s, 8).toFloat();
    String date = field(s, 9);
    if (date.length() == 6) gps.date = date;
  } else if (type == "GSV") {
    handleGsv(s);
  } else if (type == "GSA") {
    handleGsa(s);
  }
}

void gpsBegin() {
  Serial1.setRxBufferSize(1024);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

void gpsUpdate() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '$') line = "$";
    else if (c == '\r' || c == '\n') {
      if (line.length() > 1) handleSentence(line);
      line = "";
    } else if (line.length() && line.length() < 120) line += c;
  }
}

int inViewTotal() { return gps.inView[0] + gps.inView[1] + gps.inView[2] + gps.inView[3]; }
