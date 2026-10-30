// Persistent settings and waypoints (NVS) and the GPS track log (LittleFS).

#include <LittleFS.h>
#include <Preferences.h>

#include "state.h"

static const char *TRACK_PATH = "/track.bin";
static const size_t TRACK_MAX_BYTES = 1200000; // ~85k points, ~120 h at one per 5 s
static const uint32_t TRACK_EVERY_MS = 5000;
static const double TRACK_MIN_MOVE_M = 5;

struct __attribute__((packed)) TrackPoint {
  int32_t lat, lon; // 1e-7 degrees
  int16_t alt;      // metres
  uint32_t t;       // unix time
};

static Preferences prefs;
static bool fsOk = false;
static size_t numTrack = 0;
static double lastLat = NAN, lastLon = NAN;

void storageBegin() {
  prefs.begin("buddy", false);
  if (prefs.getBytesLength("settings") == sizeof(Settings)) prefs.getBytes("settings", &settings, sizeof settings);
  numWaypoints = min((int)prefs.getUChar("nwp", 0), MAX_WAYPOINTS);
  if (numWaypoints) prefs.getBytes("wps", waypoints, sizeof(Waypoint) * numWaypoints);

  fsOk = LittleFS.begin(true); // formats the partition on first boot
  if (fsOk && LittleFS.exists(TRACK_PATH)) {
    File f = LittleFS.open(TRACK_PATH, "r");
    numTrack = f.size() / sizeof(TrackPoint);
    f.close();
  }
  Serial.printf("Storage: fs=%d track=%u pts, %d waypoints\n", fsOk, (unsigned)numTrack, numWaypoints);
}

void saveSettings() { prefs.putBytes("settings", &settings, sizeof settings); }

void saveWaypoints() {
  prefs.putUChar("nwp", numWaypoints);
  prefs.putBytes("wps", waypoints, sizeof(Waypoint) * max(numWaypoints, 1));
}

bool addWaypoint(const char *name) {
  if (!gps.fix) return false;
  if (numWaypoints == MAX_WAYPOINTS) { // drop the oldest
    memmove(waypoints, waypoints + 1, sizeof(Waypoint) * (MAX_WAYPOINTS - 1));
    numWaypoints--;
  }
  Waypoint &w = waypoints[numWaypoints++];
  if (name && *name) strlcpy(w.name, name, sizeof w.name);
  else snprintf(w.name, sizeof w.name, "M%d", numWaypoints);
  w.lat = gps.lat;
  w.lon = gps.lon;
  saveWaypoints();
  blePublishWaypoints();
  return true;
}

void deleteWaypoint(int idx) {
  if (idx < 0 || idx >= numWaypoints) return;
  memmove(waypoints + idx, waypoints + idx + 1, sizeof(Waypoint) * (numWaypoints - idx - 1));
  numWaypoints--;
  saveWaypoints();
  blePublishWaypoints();
}

void renameWaypoint(int idx, const char *name) {
  if (idx < 0 || idx >= numWaypoints || !name || !*name) return;
  strlcpy(waypoints[idx].name, name, sizeof waypoints[idx].name);
  saveWaypoints();
  blePublishWaypoints();
}

void trackUpdate() {
  static uint32_t lastAt = 0;
  if (!fsOk || !gps.fix || millis() - lastAt < TRACK_EVERY_MS) return;
  lastAt = millis();
  uint32_t t = gpsUnixTime();
  if (!t || (numTrack + 1) * sizeof(TrackPoint) > TRACK_MAX_BYTES) return;
  if (!isnan(lastLat) && distM(lastLat, lastLon, gps.lat, gps.lon) < TRACK_MIN_MOVE_M) return;
  TrackPoint p = {(int32_t)lround(gps.lat * 1e7), (int32_t)lround(gps.lon * 1e7), (int16_t)lround(gps.alt), t};
  File f = LittleFS.open(TRACK_PATH, "a");
  if (!f) return;
  f.write((const uint8_t *)&p, sizeof p);
  f.close();
  numTrack++;
  lastLat = gps.lat;
  lastLon = gps.lon;
}

size_t trackPoints() { return numTrack; }

void trackClear() {
  if (fsOk) LittleFS.remove(TRACK_PATH);
  numTrack = 0;
  lastLat = lastLon = NAN;
}

bool trackStream(size_t &cursor, String &line, int maxPoints) {
  if (!fsOk || cursor >= numTrack) return false;
  File f = LittleFS.open(TRACK_PATH, "r");
  if (!f) return false;
  f.seek(cursor * sizeof(TrackPoint));
  line = "{\"t\":\"trkp\",\"p\":[";
  int n = 0;
  TrackPoint p;
  while (n < maxPoints && f.read((uint8_t *)&p, sizeof p) == sizeof p) {
    if (n++) line += ',';
    char b[64];
    snprintf(b, sizeof b, "[%.7f,%.7f,%d,%lu]", p.lat / 1e7, p.lon / 1e7, p.alt, (unsigned long)p.t);
    line += b;
  }
  f.close();
  line += "]}";
  cursor += n;
  return n > 0;
}
