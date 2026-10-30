// BLE link to the companion website (Web Bluetooth).
// One service, two characteristics:
//   data (read + notify): newline-terminated JSON lines, each tagged with "t",
//                         split to the connection's MTU
//   cmd  (write):         one JSON command per write
// Both require an authenticated (passkey) bond; the passkey shows on the OLED.

#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include "state.h"

static const char *SVC_UUID = "6b1e0001-5d4a-4c1b-9a3e-6c2f0b8d7a10";
static const char *DATA_UUID = "6b1e0002-5d4a-4c1b-9a3e-6c2f0b8d7a10";
static const char *CMD_UUID = "6b1e0003-5d4a-4c1b-9a3e-6c2f0b8d7a10";

static bool enabled = false;
static NimBLEServer *server = nullptr;
static NimBLECharacteristic *dataChr = nullptr;

static volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool authed = false;
static volatile uint16_t mtu = 23;
static volatile bool pairing = false;
static volatile uint32_t passkey = 0;
static volatile bool justAuthed = false;

// Commands arrive on the NimBLE task; loop() executes them.
static portMUX_TYPE cmdLock = portMUX_INITIALIZER_UNLOCKED;
static String cmdQueue[6];
static int cmdCount = 0;

// Outgoing lines, drained a few per loop so NimBLE's buffers keep up.
static String outQueue[48];
static int outHead = 0, outCount = 0;
static size_t outOffset = 0; // bytes of the head line already sent

static size_t trackCursor = 0;
static bool trackStreaming = false;

// ---------------------------------------------------------------- callbacks

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    connHandle = info.getConnHandle();
    authed = false;
    mtu = 23;
    Serial.printf("BLE connect %s\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    authed = false;
    pairing = false;
    Serial.printf("BLE disconnect (%d)\n", reason);
    if (enabled) NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t m, NimBLEConnInfo &info) override { mtu = m; }
  uint32_t onPassKeyDisplay() override {
    passkey = esp_random() % 1000000;
    pairing = true;
    Serial.printf("BLE pairing, passkey %06lu\n", (unsigned long)passkey);
    return passkey;
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    pairing = false;
    if (info.isEncrypted() && info.isAuthenticated()) {
      authed = true;
      justAuthed = true;
      Serial.println("BLE authenticated");
    } else {
      Serial.println("BLE pairing failed");
      server->disconnect(info);
    }
  }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    if (!info.isAuthenticated()) return;
    String v = c->getValue().c_str();
    portENTER_CRITICAL(&cmdLock);
    if (cmdCount < 6) cmdQueue[cmdCount++] = v;
    portEXIT_CRITICAL(&cmdLock);
  }
};

static ServerCallbacks serverCallbacks;
static CmdCallbacks cmdCallbacks;

// ---------------------------------------------------------------- publishing

void blePublish(const String &line) {
  if (!enabled || !authed) return;
  if (outCount == 48) { // drop the oldest rather than block
    outHead = (outHead + 1) % 48;
    outCount--;
    outOffset = 0;
  }
  outQueue[(outHead + outCount++) % 48] = line + "\n";
}

static const char *msgStateName(MsgState s) {
  switch (s) {
    case MSG_RX: return "rx";
    case MSG_WAIT: return "wait";
    case MSG_OK: return "ok";
    default: return "fail";
  }
}

void blePublishMessage(const Message &m) {
  blePublish("{\"t\":\"msg\",\"from\":\"" + idHex(m.from) + "\",\"me\":" + (m.from == myId ? 1 : 0) +
             ",\"seq\":" + m.seq + ",\"st\":\"" + msgStateName(m.state) + "\",\"sos\":" + (m.sos ? 1 : 0) +
             ",\"age\":" + (millis() - m.at) / 1000 + ",\"text\":" + jsonStr(m.text.c_str()) + "}");
}

void blePublishPeer(const Peer &p) {
  char b[220];
  snprintf(b, sizeof b,
           "{\"t\":\"peer\",\"id\":\"%04X\",\"fix\":%d,\"lat\":%.7f,\"lon\":%.7f,\"sats\":%d,\"bat\":%d,"
           "\"hdop\":%.1f,\"rssi\":%.0f,\"snr\":%.1f,\"age\":%lu}",
           p.id, p.fix, p.lat, p.lon, p.sats, p.bat, p.hdop, p.rssi, p.snr,
           (unsigned long)((millis() - p.lastHeard) / 1000));
  blePublish(b);
}

void blePublishWaypoints() {
  String s = "{\"t\":\"wps\",\"w\":[";
  for (int i = 0; i < numWaypoints; i++) {
    char b[64];
    snprintf(b, sizeof b, ",%.7f,%.7f]", waypoints[i].lat, waypoints[i].lon);
    s += (i ? ",[" : "[") + jsonStr(waypoints[i].name) + b;
  }
  blePublish(s + "]}");
}

void blePublishSettings() {
  char b[160];
  snprintf(b, sizeof b,
           "{\"t\":\"cfg\",\"leashM\":%u,\"leashMin\":%u,\"screenS\":%u,\"northRight\":%d,\"bt\":%d,\"trk\":%u}",
           settings.leashM, settings.leashMin, settings.screenS, settings.northRight, settings.bleOn,
           (unsigned)trackPoints());
  blePublish(b);
}

void blePublishAlert(const char *kind, uint16_t id, const String &text) {
  blePublish(String("{\"t\":\"alert\",\"kind\":\"") + kind + "\",\"id\":\"" + idHex(id) +
             "\",\"text\":" + jsonStr(text.c_str()) + "}");
}

static void publishSelf() {
  char b[400];
  snprintf(b, sizeof b,
           "{\"t\":\"self\",\"id\":\"%04X\",\"fix\":%d,\"lat\":%.7f,\"lon\":%.7f,\"alt\":%.1f,\"spd\":%.1f,"
           "\"crs\":%.0f,\"hdop\":%.1f,\"used\":%d,\"view\":[%d,%d,%d,%d],\"utc\":\"%s\",\"date\":\"%s\","
           "\"bat\":%d,\"mv\":%d,\"gpsOk\":%d}",
           myId, gps.fix, gps.fix ? gps.lat : 0, gps.fix ? gps.lon : 0, gps.alt, gps.speedKmh, gps.course, gps.hdop,
           gps.sats, gps.inView[0], gps.inView[1], gps.inView[2], gps.inView[3], gps.time.c_str(), gps.date.c_str(),
           batteryPct, batteryMv, millis() - gps.lastSentence < 3000);
  blePublish(b);
}

static void publishSky() {
  String s = "{\"t\":\"sky\",\"s\":[";
  for (int i = 0; i < gps.numSky; i++) {
    const Sat &k = gps.sky[i];
    char b[40];
    snprintf(b, sizeof b, "%s[%u,%u,%u,%u,%u,%d]", i ? "," : "", k.sys, k.prn, k.el, k.az, k.snr, k.used);
    s += b;
  }
  blePublish(s + "]}");
}

// Everything a freshly connected page needs.
static void publishAll() {
  publishSelf();
  publishSky();
  blePublishSettings();
  blePublishWaypoints();
  for (int i = 0; i < numPeers; i++) blePublishPeer(peers[i]);
  for (int i = numMsgs - 1; i >= 0; i--) blePublishMessage(msgs[i]);
}

// ---------------------------------------------------------------- commands

static void runCommand(const String &raw) {
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return;
  String c = doc["c"] | "";
  Serial.printf("BLE cmd %s\n", raw.c_str());

  if (c == "hello") publishAll();
  else if (c == "send") {
    String text = doc["text"] | "";
    text.toUpperCase();
    String clean;
    for (char ch : text)
      if (ch >= 32 && ch < 127) clean += ch;
    clean.trim();
    if (clean.length()) sendText(clean.substring(0, MAX_TEXT), false);
  } else if (c == "sos") sendText("SOS", true);
  else if (c == "sosack") sosAcknowledge();
  else if (c == "scan") {
    String id = doc["id"] | "me";
    uint8_t what = doc["what"] | 3;
    if (id == "me") {
      if (!scanStart(myId, what)) blePublishAlert("info", myId, "Scanner busy, try again shortly");
    } else sendScanRequest(id == "all" ? 0xFFFF : (uint16_t)strtoul(id.c_str(), nullptr, 16), what);
  } else if (c == "mark") {
    if (!addWaypoint(doc["name"] | "")) blePublishAlert("info", myId, "No GPS fix, can't mark");
  } else if (c == "delmark") deleteWaypoint(doc["i"] | -1);
  else if (c == "renmark") renameWaypoint(doc["i"] | -1, doc["name"] | "");
  else if (c == "set") {
    if (doc["leashM"].is<int>()) settings.leashM = doc["leashM"];
    if (doc["leashMin"].is<int>()) settings.leashMin = doc["leashMin"];
    if (doc["screenS"].is<int>()) settings.screenS = doc["screenS"];
    if (!doc["northRight"].isNull()) settings.northRight = doc["northRight"].as<int>();
    saveSettings();
    blePublishSettings();
  } else if (c == "gpx") {
    trackCursor = 0;
    trackStreaming = true;
    blePublish("{\"t\":\"trk\",\"n\":" + String((unsigned)trackPoints()) + "}");
  } else if (c == "gpxclear") {
    trackClear();
    blePublishSettings();
  } else if (c == "forget") bleForgetBonds();
}

// ---------------------------------------------------------------- lifecycle

static void start() {
  NimBLEDevice::init(("Buddy-" + idHex(myId)).c_str());
  NimBLEDevice::setPower(3);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true); // bond, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  NimBLEService *svc = server->createService(SVC_UUID);
  dataChr = svc->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                                                     NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::NOTIFY);
  dataChr->setValue("{\"t\":\"hello\"}");
  NimBLECharacteristic *cmd = svc->createCharacteristic(
      CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 512);
  cmd->setCallbacks(&cmdCallbacks);
  server->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(("Buddy-" + idHex(myId)).c_str());
  adv->addServiceUUID(SVC_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("BLE on, advertising");
}

static void stop() {
  connHandle = BLE_HS_CONN_HANDLE_NONE;
  authed = false;
  pairing = false;
  server = nullptr;
  dataChr = nullptr;
  outCount = 0;
  outOffset = 0;
  trackStreaming = false;
  // A running Wi-Fi/BLE survey still needs the stack; scan.cpp tears it down when done.
  if (!scanBusy()) NimBLEDevice::deinit(true);
  else {
    NimBLEDevice::stopAdvertising();
    if (NimBLEServer *s = NimBLEDevice::getServer())
      for (uint8_t i = 0; i < s->getConnectedCount(); i++) s->disconnect(s->getPeerInfo(i));
  }
  Serial.println("BLE off");
}

void bleSetEnabled(bool on) {
  if (on == enabled) return;
  enabled = on;
  if (on) start();
  else stop();
}

bool bleIsOn() { return enabled; }
bool bleConnected() { return enabled && connHandle != BLE_HS_CONN_HANDLE_NONE && authed; }

bool blePairing(uint32_t *code) {
  if (code) *code = passkey;
  return enabled && pairing;
}

void bleForgetBonds() {
  if (!NimBLEDevice::isInitialized()) {
    NimBLEDevice::init("");
    NimBLEDevice::deleteAllBonds();
    if (!scanBusy()) NimBLEDevice::deinit(true);
  } else {
    NimBLEDevice::deleteAllBonds();
    if (server)
      for (uint8_t i = 0; i < server->getConnectedCount(); i++) server->disconnect(server->getPeerInfo(i));
  }
  Serial.println("BLE bonds cleared");
}

void bleUpdate() {
  if (!enabled) return;

  String pending[6];
  int n;
  portENTER_CRITICAL(&cmdLock);
  n = cmdCount;
  for (int i = 0; i < n; i++) pending[i] = cmdQueue[i];
  cmdCount = 0;
  portEXIT_CRITICAL(&cmdLock);
  for (int i = 0; i < n; i++) runCommand(pending[i]);

  // Advertising can stop after a forced disconnect (e.g. bonds cleared); restart it
  static uint32_t lastAdvCheck = 0;
  if (millis() - lastAdvCheck > 2000) {
    lastAdvCheck = millis();
    if (server && !server->getConnectedCount() && !NimBLEDevice::getAdvertising()->isAdvertising()) {
      Serial.println("BLE advertising restarted");
      NimBLEDevice::startAdvertising();
    }
  }

  if (!authed || connHandle == BLE_HS_CONN_HANDLE_NONE) return;
  if (justAuthed) {
    justAuthed = false;
    publishAll();
  }

  uint32_t now = millis();
  static uint32_t lastSelf = 0, lastSky = 0;
  if (now - lastSelf >= 1000) {
    lastSelf = now;
    publishSelf();
  }
  if (now - lastSky >= 5000) {
    lastSky = now;
    publishSky();
  }

  if (trackStreaming && outCount < 8) {
    String line;
    if (trackStream(trackCursor, line, 6)) blePublish(line);
    else {
      trackStreaming = false;
      blePublish("{\"t\":\"trkend\"}");
    }
  }

  // Drain: split each line to MTU-3 sized notifications.
  for (int budget = 6; budget > 0 && outCount; budget--) {
    const String &line = outQueue[outHead];
    size_t chunk = max(20, mtu - 3);
    size_t len = min(chunk, line.length() - outOffset);
    if (!dataChr->notify((const uint8_t *)line.c_str() + outOffset, len, connHandle)) break; // retry next loop
    outOffset += len;
    if (outOffset >= line.length()) {
      outOffset = 0;
      outQueue[outHead] = "";
      outHead = (outHead + 1) % 48;
      outCount--;
    }
  }
}
