// On-demand Wi-Fi + BLE survey, run for a local phone or a remote board.
// Non-blocking: Wi-Fi uses the async scan, BLE uses NimBLE's timed scan.

#include <NimBLEDevice.h>
#include <WiFi.h>

#include "state.h"

static const uint32_t MIN_INTERVAL_MS = 15000;
static const uint32_t BLE_SCAN_MS = 4000;

enum ScanState { SCAN_IDLE, SCAN_WIFI_RUNNING, SCAN_BLE_RUNNING };
static ScanState state = SCAN_IDLE;
static uint16_t requester;
static uint8_t what;
static uint32_t stateAt, lastScanAt;
static bool everScanned = false, ownsStack = false;

static WifiEntry wifi[SCAN_MAX];
static BleEntry ble[SCAN_MAX];
static int numWifi, numBle;

static String wifiJson(const WifiEntry *w, int n) {
  String e = "[";
  for (int i = 0; i < n; i++) {
    if (i) e += ',';
    e += "[" + String(w[i].rssi) + "," + w[i].ch + "," + w[i].auth + "," + jsonStr(w[i].ssid) + "]";
  }
  return e + "]";
}

static String bleJson(const BleEntry *b, int n) {
  String e = "[";
  for (int i = 0; i < n; i++) {
    if (i) e += ',';
    e += "[" + String(b[i].rssi) + "," + (b[i].company == 0xFFFF ? -1 : (int)b[i].company) + "," +
         jsonStr(b[i].name) + "]";
  }
  return e + "]";
}

static void publishPart(uint16_t from, uint8_t kind, int part, int total, const String &entries) {
  blePublish("{\"t\":\"scan\",\"from\":\"" + idHex(from) + "\",\"kind\":\"" + (kind == SCAN_WIFI ? "wifi" : "ble") +
             "\",\"part\":" + part + ",\"total\":" + total + ",\"e\":" + entries + "}");
}

static void startBle() {
  ownsStack = !NimBLEDevice::isInitialized();
  if (ownsStack) NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true); // scan responses usually carry the device name
  scan->setInterval(100);
  scan->setWindow(90);
  scan->setDuplicateFilter(1);
  scan->clearResults();
  scan->start(BLE_SCAN_MS, false, true);
  state = SCAN_BLE_RUNNING;
  stateAt = millis();
}

static void finish() {
  state = SCAN_IDLE;
  Serial.printf("Scan done: %d Wi-Fi, %d BLE\n", numWifi, numBle);
  if (requester == myId) {
    if (what & SCAN_WIFI) publishPart(myId, SCAN_WIFI, 0, 1, wifiJson(wifi, numWifi));
    if (what & SCAN_BLE) publishPart(myId, SCAN_BLE, 0, 1, bleJson(ble, numBle));
  } else {
    sendScanResults(requester, wifi, (what & SCAN_WIFI) ? numWifi : 0, ble, (what & SCAN_BLE) ? numBle : 0);
  }
}

bool scanStart(uint16_t req, uint8_t w) {
  if (state != SCAN_IDLE || (everScanned && millis() - lastScanAt < MIN_INTERVAL_MS) || !(w & 3)) return false;
  requester = req;
  what = w;
  numWifi = numBle = 0;
  everScanned = true;
  lastScanAt = millis();
  Serial.printf("Scan start for %04X (what=%u)\n", req, w);
  if (w & SCAN_WIFI) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.scanNetworks(true, true); // async, include hidden networks
    state = SCAN_WIFI_RUNNING;
    stateAt = millis();
  } else {
    startBle();
  }
  return true;
}

bool scanBusy() { return state != SCAN_IDLE; }

void scanUpdate() {
  if (state == SCAN_WIFI_RUNNING) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING && millis() - stateAt < 12000) return;
    for (int i = 0; i < n; i++) { // keep the strongest SCAN_MAX
      WifiEntry e = {(int8_t)WiFi.RSSI(i), (uint8_t)WiFi.channel(i), (uint8_t)WiFi.encryptionType(i), {0}};
      strlcpy(e.ssid, WiFi.SSID(i).c_str(), sizeof e.ssid);
      int pos = numWifi;
      while (pos > 0 && wifi[pos - 1].rssi < e.rssi) pos--;
      if (pos >= SCAN_MAX) continue;
      for (int j = min(numWifi, SCAN_MAX - 1); j > pos; j--) wifi[j] = wifi[j - 1];
      wifi[pos] = e;
      if (numWifi < SCAN_MAX) numWifi++;
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    if (what & SCAN_BLE) startBle();
    else finish();
  } else if (state == SCAN_BLE_RUNNING) {
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning() && millis() - stateAt < BLE_SCAN_MS + 3000) return;
    scan->stop();
    NimBLEScanResults results = scan->getResults();
    for (int i = 0; i < results.getCount(); i++) {
      const NimBLEAdvertisedDevice *d = results.getDevice(i);
      BleEntry e = {d->getRSSI(), 0xFFFF, {0}};
      if (d->haveName()) strlcpy(e.name, d->getName().c_str(), sizeof e.name);
      std::string mfr = d->getManufacturerData();
      if (mfr.size() >= 2) e.company = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
      int pos = numBle;
      while (pos > 0 && ble[pos - 1].rssi < e.rssi) pos--;
      if (pos >= SCAN_MAX) continue;
      for (int j = min(numBle, SCAN_MAX - 1); j > pos; j--) ble[j] = ble[j - 1];
      ble[pos] = e;
      if (numBle < SCAN_MAX) numBle++;
    }
    scan->clearResults();
    if (ownsStack && !bleIsOn()) NimBLEDevice::deinit(true);
    finish();
  }
}

// A part of a remote board's results, packed as in sendScanResults().
void scanResultReceived(uint16_t from, uint8_t kind, uint8_t part, uint8_t total, const uint8_t *d, size_t len) {
  WifiEntry w[SCAN_MAX];
  BleEntry b[SCAN_MAX];
  int n = 0;
  size_t i = 0;
  while (i + 4 <= len && n < SCAN_MAX) {
    size_t slen = d[i + 3];
    if (i + 4 + slen > len) break;
    if (kind == SCAN_WIFI) {
      w[n] = {(int8_t)d[i], d[i + 1], d[i + 2], {0}};
      memcpy(w[n].ssid, d + i + 4, min(slen, sizeof w[n].ssid - 1));
    } else {
      b[n] = {(int8_t)d[i], (uint16_t)(d[i + 1] | d[i + 2] << 8), {0}};
      memcpy(b[n].name, d + i + 4, min(slen, sizeof b[n].name - 1));
    }
    n++;
    i += 4 + slen;
  }
  publishPart(from, kind, part, total, kind == SCAN_WIFI ? wifiJson(w, n) : bleJson(b, n));
}
