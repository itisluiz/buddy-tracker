// LoRa link: position beacons, acknowledged messages, remote scan requests.
// Transmits are non-blocking (startTransmit + DIO1) so the button and display
// stay responsive while a long packet is on air.

#include "state.h"

static const float LORA_FREQ = 915.0;
static const float LORA_BW = 125.0;
static const uint8_t LORA_SF = 9;
static const uint8_t LORA_CR = 5;
static const uint8_t LORA_SYNC = 0x12;
static const int8_t LORA_POWER = 14;
static const uint32_t BEACON_MS = 5000;
static const uint32_t BEACON_NOFIX_MS = 30000; // keep the air quiet while the GPS is still acquiring
static const uint32_t RETRY_MS = 3000;
static const uint8_t MAX_TRIES = 2;

static SX1262 radio = new Module(8, 14, 12, 13); // NSS, DIO1, RST, BUSY

// ---------------------------------------------------------------- packets

static const uint8_t MAGIC = 0xB8; // bumped from 0xB7: packet layouts changed
enum : uint8_t { PKT_POS = 1, PKT_MSG = 2, PKT_ACK = 3, PKT_SCAN_REQ = 4, PKT_SCAN_RES = 5 };
static const uint8_t MSG_FLAG_SOS = 1;

struct __attribute__((packed)) Header {
  uint8_t magic, type;
  uint16_t id;
  uint8_t seq;
};

struct __attribute__((packed)) PosPacket {
  Header h;
  uint8_t fix, sats;
  uint8_t bat;    // 255 = unknown
  uint8_t hdop10; // HDOP x10, 255 = unknown
  int32_t lat, lon; // 1e-7 degrees
};

struct __attribute__((packed)) MsgPacket {
  Header h;
  uint8_t flags, len; // text follows
};

struct __attribute__((packed)) AckPacket {
  Header h;
  uint16_t to;
  uint8_t ackSeq;
};

struct __attribute__((packed)) ScanReqPacket {
  Header h;
  uint16_t target; // 0xFFFF = everyone
  uint8_t what;
};

struct __attribute__((packed)) ScanResHeader {
  Header h;
  uint16_t reqBy;
  uint8_t kind, part, total; // entries follow
};

static const size_t SCAN_PART_BYTES = 120;

// ---------------------------------------------------------------- tx queue

struct TxItem {
  bool used;
  uint8_t len;
  uint8_t data[sizeof(ScanResHeader) + SCAN_PART_BYTES + 40];
  uint32_t notBefore;
};
static TxItem txq[16];

static volatile bool radioFlag = false;
static void IRAM_ATTR onRadio() { radioFlag = true; }

static bool txActive = false;
static uint32_t txStartedAt = 0;
static uint8_t txSeq = 0;
static uint32_t nextBeacon = 0;

static void enqueue(const void *data, size_t len, uint32_t delayMs = 0) {
  for (auto &it : txq) {
    if (it.used) continue;
    it.used = true;
    it.len = min(len, sizeof(it.data));
    memcpy(it.data, data, it.len);
    it.notBefore = millis() + delayMs;
    return;
  }
  Serial.println("TX queue full, dropping packet");
}

static Header header(uint8_t type, uint8_t seq) { return {MAGIC, type, myId, seq}; }

// ---------------------------------------------------------------- sending

static void queueBeacon() {
  PosPacket p = {header(PKT_POS, txSeq++), gps.fix, (uint8_t)gps.sats,
                 (uint8_t)(batteryPct < 0 ? 255 : batteryPct),
                 (uint8_t)(gps.hdop >= 25 ? 255 : lround(gps.hdop * 10)),
                 gps.fix ? (int32_t)lround(gps.lat * 1e7) : 0, gps.fix ? (int32_t)lround(gps.lon * 1e7) : 0};
  enqueue(&p, sizeof p);
}

void sendBeaconSoon() { nextBeacon = millis() + random(100, 500); }

static void queueMessage(const Message &m, uint32_t delayMs) {
  uint8_t buf[sizeof(MsgPacket) + MAX_TEXT];
  MsgPacket p = {header(PKT_MSG, m.seq), (uint8_t)(m.sos ? MSG_FLAG_SOS : 0), (uint8_t)min(m.text.length(), MAX_TEXT)};
  memcpy(buf, &p, sizeof p);
  memcpy(buf + sizeof p, m.text.c_str(), p.len);
  enqueue(buf, sizeof p + p.len, delayMs);
}

static void addMessage(const Message &m) {
  for (int i = min(numMsgs, MAX_MSGS - 1); i > 0; i--) msgs[i] = msgs[i - 1];
  msgs[0] = m;
  if (numMsgs < MAX_MSGS) numMsgs++;
}

void sendText(const String &text, bool sos) {
  Message m = {myId, txSeq++, text.substring(0, MAX_TEXT), millis(), MSG_WAIT, sos, 1,
               millis() + RETRY_MS + random(0, 1000)};
  addMessage(m);
  queueMessage(m, 0);
  blePublishMessage(m);
  if (sos) sendBeaconSoon(); // make sure everyone has our latest position
  Serial.printf("TX msg #%u \"%s\"%s\n", m.seq, m.text.c_str(), sos ? " [SOS]" : "");
}

void sendScanRequest(uint16_t target, uint8_t what) {
  ScanReqPacket p = {header(PKT_SCAN_REQ, txSeq++), target, what};
  enqueue(&p, sizeof p);
  Serial.printf("TX scan request -> %04X\n", target);
}

// Pack entries into <=SCAN_PART_BYTES parts; kind 1 = Wi-Fi, 2 = BLE.
template <typename F>
static void queueScanParts(uint16_t requester, uint8_t kind, int n, F packEntry, uint32_t &delay) {
  uint8_t parts[8][SCAN_PART_BYTES];
  size_t partLen[8] = {0};
  int numParts = 1;
  for (int i = 0; i < n; i++) {
    uint8_t entry[64];
    size_t len = packEntry(i, entry);
    if (partLen[numParts - 1] + len > SCAN_PART_BYTES) {
      if (numParts == 8) break;
      numParts++;
    }
    memcpy(parts[numParts - 1] + partLen[numParts - 1], entry, len);
    partLen[numParts - 1] += len;
  }
  for (int p = 0; p < numParts; p++) {
    uint8_t buf[sizeof(ScanResHeader) + SCAN_PART_BYTES];
    ScanResHeader h = {header(PKT_SCAN_RES, txSeq++), requester, kind, (uint8_t)p, (uint8_t)numParts};
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, parts[p], partLen[p]);
    enqueue(buf, sizeof h + partLen[p], delay);
    delay += 250 + random(0, 300);
  }
}

void sendScanResults(uint16_t requester, const WifiEntry *w, int nw, const BleEntry *b, int nb) {
  uint32_t delay = random(100, 400);
  queueScanParts(requester, SCAN_WIFI, nw, [&](int i, uint8_t *e) {
    size_t len = strlen(w[i].ssid);
    e[0] = (uint8_t)w[i].rssi, e[1] = w[i].ch, e[2] = w[i].auth, e[3] = len;
    memcpy(e + 4, w[i].ssid, len);
    return 4 + len;
  }, delay);
  queueScanParts(requester, SCAN_BLE, nb, [&](int i, uint8_t *e) {
    size_t len = strlen(b[i].name);
    e[0] = (uint8_t)b[i].rssi, e[1] = b[i].company & 0xFF, e[2] = b[i].company >> 8, e[3] = len;
    memcpy(e + 4, b[i].name, len);
    return 4 + len;
  }, delay);
}

// ---------------------------------------------------------------- receiving

Peer *findPeer(uint16_t id) {
  for (int i = 0; i < numPeers; i++)
    if (peers[i].id == id) return &peers[i];
  return nullptr;
}

static Peer &peerFor(uint16_t id) {
  if (Peer *p = findPeer(id)) return *p;
  int slot = numPeers;
  if (numPeers < MAX_PEERS) numPeers++;
  else { // replace the longest-silent peer
    slot = 0;
    for (int i = 1; i < MAX_PEERS; i++)
      if (peers[i].lastHeard < peers[slot].lastHeard) slot = i;
  }
  peers[slot] = {id, false, 0, -1, 99, 0, 0, 0, 0, 0, -1, false, false};
  return peers[slot];
}

static void handlePacket(const uint8_t *buf, size_t len) {
  if (len < sizeof(Header) || buf[0] != MAGIC) return;
  Header h;
  memcpy(&h, buf, sizeof h);
  if (h.id == myId) return;

  Peer &peer = peerFor(h.id);
  peer.rssi = radio.getRSSI();
  peer.snr = radio.getSNR();
  peer.lastHeard = millis();
  peer.silentAlerted = false;

  switch (h.type) {
    case PKT_POS: {
      if (len < sizeof(PosPacket)) return;
      PosPacket p;
      memcpy(&p, buf, sizeof p);
      peer.fix = p.fix;
      peer.sats = p.sats;
      peer.bat = p.bat == 255 ? -1 : p.bat;
      peer.hdop = p.hdop10 == 255 ? 99 : p.hdop10 / 10.0f;
      if (p.fix) {
        peer.lat = p.lat / 1e7;
        peer.lon = p.lon / 1e7;
      }
      blePublishPeer(peer);
      Serial.printf("RX pos %04X fix=%d %.6f,%.6f bat=%d rssi=%.0f snr=%.1f\n", h.id, p.fix, peer.lat, peer.lon,
                    peer.bat, peer.rssi, peer.snr);
      break;
    }
    case PKT_MSG: {
      if (len < sizeof(MsgPacket)) return;
      MsgPacket p;
      memcpy(&p, buf, sizeof p);
      AckPacket ack = {header(PKT_ACK, txSeq++), h.id, h.seq};
      enqueue(&ack, sizeof ack, random(150, 500)); // ack retries too, the first ack may have been lost
      if (peer.lastMsgSeq == h.seq) return;        // duplicate from a retry
      peer.lastMsgSeq = h.seq;
      String text;
      for (size_t i = 0; i < p.len && sizeof p + i < len; i++) {
        char c = (char)buf[sizeof p + i];
        if (c >= 32 && c < 127) text += c;
      }
      bool sos = p.flags & MSG_FLAG_SOS;
      Message m = {h.id, h.seq, text, millis(), MSG_RX, sos, 0, 0};
      addMessage(m);
      blePublishMessage(m);
      if (sos) alertSos(h.id);
      else {
        showBanner(idHex(h.id) + ": " + text);
        ledPlay(text);
      }
      Serial.printf("RX msg %04X #%u \"%s\"%s rssi=%.0f\n", h.id, h.seq, text.c_str(), sos ? " [SOS]" : "", peer.rssi);
      break;
    }
    case PKT_ACK: {
      if (len < sizeof(AckPacket)) return;
      AckPacket a;
      memcpy(&a, buf, sizeof a);
      if (a.to != myId) return;
      for (int i = 0; i < numMsgs; i++)
        if (msgs[i].from == myId && msgs[i].seq == a.ackSeq && msgs[i].state != MSG_OK) {
          msgs[i].state = MSG_OK;
          blePublishMessage(msgs[i]);
          Serial.printf("RX ack #%u from %04X\n", a.ackSeq, h.id);
        }
      break;
    }
    case PKT_SCAN_REQ: {
      if (len < sizeof(ScanReqPacket)) return;
      ScanReqPacket r;
      memcpy(&r, buf, sizeof r);
      if (r.target != myId && r.target != 0xFFFF) return;
      Serial.printf("RX scan request from %04X\n", h.id);
      if (scanStart(h.id, r.what)) showBanner(idHex(h.id) + " scans here");
      break;
    }
    case PKT_SCAN_RES: {
      if (len < sizeof(ScanResHeader)) return;
      ScanResHeader r;
      memcpy(&r, buf, sizeof r);
      if (r.reqBy != myId) return;
      Serial.printf("RX scan part %u/%u kind=%u from %04X\n", r.part + 1, r.total, r.kind, h.id);
      scanResultReceived(h.id, r.kind, r.part, r.total, buf + sizeof r, len - sizeof r);
      break;
    }
  }
}

// ---------------------------------------------------------------- loop

void radioBegin() {
  SPI.begin(9, 11, 10, 8);
  int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER, 8, 1.8);
  if (st == RADIOLIB_ERR_NONE) st = radio.setDio2AsRfSwitch(true);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio init failed: %d\n", st);
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.setCursor(0, 20);
    oled.printf("Radio init failed %d", st);
    oled.sendBuffer();
    while (true) delay(1000);
  }
  radio.setPacketReceivedAction(onRadio); // DIO1: RX done or TX done
  radio.startReceive();
}

void radioUpdate() {
  uint32_t now = millis();

  if (radioFlag) {
    radioFlag = false;
    if (txActive) {
      radio.finishTransmit();
      txActive = false;
    } else {
      uint8_t buf[256];
      size_t len = min((size_t)radio.getPacketLength(), sizeof buf);
      if (radio.readData(buf, len) == RADIOLIB_ERR_NONE) handlePacket(buf, len);
    }
    radio.startReceive();
  }

  if (txActive && now - txStartedAt > 5000) { // TX-done never arrived
    Serial.println("TX timeout");
    radio.finishTransmit();
    txActive = false;
    radio.startReceive();
  }

  if (now >= nextBeacon) {
    queueBeacon();
    nextBeacon = now + (gps.fix ? BEACON_MS : BEACON_NOFIX_MS) + random(0, 1000); // jitter avoids repeat collisions
  }

  for (int i = 0; i < numMsgs; i++) {
    Message &m = msgs[i];
    if (m.from != myId || m.state != MSG_WAIT || now < m.retryAt) continue;
    if (m.tries < MAX_TRIES) {
      m.tries++;
      m.retryAt = now + RETRY_MS + random(0, 1000);
      queueMessage(m, 0);
      Serial.printf("TX retry #%u\n", m.seq);
    } else {
      m.state = MSG_FAIL;
      blePublishMessage(m);
    }
  }

  if (!txActive) {
    TxItem *next = nullptr;
    for (auto &it : txq)
      if (it.used && (int32_t)(now - it.notBefore) >= 0 && (!next || (int32_t)(it.notBefore - next->notBefore) < 0))
        next = &it;
    if (next) {
      int st = radio.startTransmit(next->data, next->len);
      next->used = false;
      if (st == RADIOLIB_ERR_NONE) {
        txActive = true;
        txStartedAt = now;
      } else {
        Serial.printf("TX failed: %d\n", st);
        radio.startReceive();
      }
    }
  }
}
