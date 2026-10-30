// Shared state and module interfaces for the LoRa buddy tracker.
#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <U8g2lib.h>

// ---------------------------------------------------------------- pins

static const int VEXT_PIN = 36; // LOW = Vext rail (GPS + OLED) powered
static const int LED_PIN = 35;
static const int BUTTON_PIN = 0;  // PRG
static const int GPS_RX_PIN = 41; // wired to GPS TX
static const int GPS_TX_PIN = 42; // wired to GPS RX
static const int VBAT_PIN = 1;
static const int ADC_CTRL_PIN = 37;

// ---------------------------------------------------------------- limits

static const uint32_t STALE_MS = 30000;
static const uint32_t FORGET_MS = 600000;
static const size_t MAX_TEXT = 32;
static const int MAX_PEERS = 8;
static const int MAX_MSGS = 8;
static const int MAX_WAYPOINTS = 8;
static const int MAX_SKY = 48;
static const int SCAN_MAX = 10;

// ---------------------------------------------------------------- types

struct Sat {
  uint8_t sys; // 0 GP, 1 BD, 2 GL, 3 GA
  uint8_t prn, el;
  uint16_t az;
  uint8_t snr;
  bool used;
};

struct Gps {
  bool fix = false;
  int sats = 0;
  int inView[4] = {0, 0, 0, 0};
  double lat = NAN, lon = NAN;
  float alt = 0, hdop = 99, speedKmh = 0, course = 0;
  String time, date; // hhmmss, ddmmyy (UTC)
  uint32_t lastSentence = 0;
  Sat sky[MAX_SKY];
  int numSky = 0;
};

struct Peer {
  uint16_t id;
  bool fix;
  int sats;
  int bat; // -1 unknown
  float hdop;
  double lat, lon;
  float rssi, snr;
  uint32_t lastHeard;
  int lastMsgSeq;
  bool farAlerted, silentAlerted;
};

enum MsgState : uint8_t { MSG_RX, MSG_WAIT, MSG_OK, MSG_FAIL };

struct Message {
  uint16_t from;
  uint8_t seq;
  String text;
  uint32_t at;
  MsgState state;
  bool sos;
  uint8_t tries;
  uint32_t retryAt;
};

struct Waypoint {
  char name[12];
  double lat, lon;
};

struct Settings {
  uint16_t leashM = 0;   // 0 = off
  uint16_t leashMin = 0; // 0 = off
  uint16_t screenS = 60; // 0 = never sleep
  bool northRight = true;
  bool bleOn = false;
};

struct WifiEntry {
  int8_t rssi;
  uint8_t ch, auth;
  char ssid[33];
};

struct BleEntry {
  int8_t rssi;
  uint16_t company; // 0xFFFF = none
  char name[21];
};

// ---------------------------------------------------------------- globals

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled;
extern uint16_t myId;
extern Gps gps;
extern Peer peers[MAX_PEERS];
extern int numPeers;
extern Message msgs[MAX_MSGS]; // newest first
extern int numMsgs;
extern Waypoint waypoints[MAX_WAYPOINTS];
extern int numWaypoints;
extern Settings settings;
extern int batteryPct, batteryMv;

// ---------------------------------------------------------------- util.cpp

double distM(double lat1, double lon1, double lat2, double lon2);
double bearingDeg(double lat1, double lon1, double lat2, double lon2);
const char *compass(double brg);
String fmtDist(double m);
String fmtAge(uint32_t ms);
String jsonStr(const char *s); // quoted + escaped
String idHex(uint16_t id);
uint32_t gpsUnixTime(); // 0 if unknown

// ---------------------------------------------------------------- gps.cpp

void gpsBegin();
void gpsUpdate();
int inViewTotal();

// ---------------------------------------------------------------- radio.cpp

void radioBegin();
void radioUpdate();
void sendText(const String &text, bool sos);
void sendScanRequest(uint16_t target, uint8_t what);
void sendScanResults(uint16_t requester, const WifiEntry *w, int nw, const BleEntry *b, int nb);
void sendBeaconSoon();
Peer *findPeer(uint16_t id);

// ---------------------------------------------------------------- display.cpp

enum Page { PAGE_MAP, PAGE_PEERS, PAGE_MSGS, PAGE_GPS, NUM_PAGES };
extern int page;
void displayBegin();
void displayUpdate();
void displayWake(); // counts as activity for the screen timeout
bool displayAsleep();
void showBanner(const String &text, uint32_t ms = 6000);
extern String holdHint;   // bottom-bar hint while the button is held
extern String draftLine;  // Morse draft being keyed, empty if none

// ---------------------------------------------------------------- ble.cpp

void bleSetEnabled(bool on);
bool bleIsOn();
bool bleConnected();
bool blePairing(uint32_t *code);
void bleUpdate();
void bleForgetBonds();
void blePublish(const String &jsonLine); // no-op unless a paired phone is connected
void blePublishMessage(const Message &m);
void blePublishPeer(const Peer &p);
void blePublishWaypoints();
void blePublishSettings();
void blePublishAlert(const char *kind, uint16_t id, const String &text);

// ---------------------------------------------------------------- scan.cpp

bool scanStart(uint16_t requester, uint8_t what); // what: bit0 Wi-Fi, bit1 BLE
bool scanBusy();
void scanUpdate();
void scanResultReceived(uint16_t from, uint8_t kind, uint8_t part, uint8_t total, const uint8_t *data, size_t len);
static const uint8_t SCAN_WIFI = 1, SCAN_BLE = 2;

// ---------------------------------------------------------------- storage.cpp

void storageBegin();
void saveSettings();
void saveWaypoints();
bool addWaypoint(const char *name); // at the current fix
void deleteWaypoint(int idx);
void renameWaypoint(int idx, const char *name);
void trackUpdate();
size_t trackPoints();
void trackClear();
bool trackStream(size_t &cursor, String &line, int maxPoints); // false when done

// ---------------------------------------------------------------- main.cpp

void alertSos(uint16_t from);
void ledPlay(const String &text);
void ledFlash(int count);
bool sosAlarm(uint16_t *from); // true while an SOS waits for acknowledgement
void sosAcknowledge();
