#include <math.h>
#include <time.h>

#include "state.h"

double distM(double lat1, double lon1, double lat2, double lon2) {
  double p1 = radians(lat1), p2 = radians(lat2);
  double dp = p2 - p1, dl = radians(lon2 - lon1);
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double p1 = radians(lat1), p2 = radians(lat2), dl = radians(lon2 - lon1);
  double y = sin(dl) * cos(p2);
  double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  return fmod(degrees(atan2(y, x)) + 360.0, 360.0);
}

const char *compass(double brg) {
  static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return pts[(int)((brg + 22.5) / 45.0) % 8];
}

String fmtDist(double m) {
  char b[12];
  if (m < 1000) snprintf(b, sizeof b, "%dm", (int)lround(m));
  else if (m < 10000) snprintf(b, sizeof b, "%.2fkm", m / 1000);
  else snprintf(b, sizeof b, "%dkm", (int)lround(m / 1000));
  return b;
}

String fmtAge(uint32_t ms) {
  char b[12];
  uint32_t s = ms / 1000;
  if (s < 60) snprintf(b, sizeof b, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(b, sizeof b, "%lum", (unsigned long)(s / 60));
  else snprintf(b, sizeof b, "%luh", (unsigned long)(s / 3600));
  return b;
}

String jsonStr(const char *s) {
  String out = "\"";
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char b[8];
      snprintf(b, sizeof b, "\\u%04x", c);
      out += b;
    } else out += c;
  }
  return out + "\"";
}

String idHex(uint16_t id) {
  char b[5];
  snprintf(b, sizeof b, "%04X", id);
  return b;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

uint32_t gpsUnixTime() {
  if (gps.time.length() < 6 || gps.date.length() < 6) return 0;
  int dd = gps.date.substring(0, 2).toInt(), mo = gps.date.substring(2, 4).toInt();
  int yy = 2000 + gps.date.substring(4, 6).toInt();
  int hh = gps.time.substring(0, 2).toInt(), mi = gps.time.substring(2, 4).toInt();
  int ss = gps.time.substring(4, 6).toInt();
  if (mo < 1 || mo > 12 || dd < 1) return 0;
  return (uint32_t)daysFromCivil(yy, mo, dd) * 86400u + hh * 3600 + mi * 60 + ss;
}
