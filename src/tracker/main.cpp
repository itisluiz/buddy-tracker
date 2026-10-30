// LoRa buddy tracker for Heltec WiFi LoRa 32 V3 + ATGM336H GPS.
// Boards share positions over LoRa and plot each other on a north-right map.
// PRG button: tap = dot, ~0.5 s = dash; release after 1.2 s = next page (or
// cancel a draft), after 3 s = Bluetooth on/off, after 6 s = forget phones.
// Keyed words SOS and MARK are commands; anything else is sent as a message.

#include "state.h"

// Morse keying
static const uint32_t DASH_MS = 250;
static const uint32_t PAGE_HOLD_MS = 1200;
static const uint32_t BT_HOLD_MS = 3000;
static const uint32_t FORGET_HOLD_MS = 6000;
static const uint32_t LETTER_GAP_MS = 600;
static const uint32_t WORD_GAP_MS = 1500;
static const uint32_t SEND_GAP_MS = 4000;
static const uint32_t LED_UNIT_MS = 120;

uint16_t myId;
Gps gps;
Peer peers[MAX_PEERS];
int numPeers = 0;
Message msgs[MAX_MSGS];
int numMsgs = 0;
Waypoint waypoints[MAX_WAYPOINTS];
int numWaypoints = 0;
Settings settings;
int batteryPct = -1, batteryMv = 0;

static bool sosActive = false;
static uint16_t sosFrom = 0;

struct Keyer {
  bool down = false;
  uint32_t pressAt = 0, releaseAt = 0, lastChange = 0;
  bool swallow = false; // press used to wake the screen or acknowledge an SOS
  String symbols, text;
} key;

struct LedPlayer {
  String pattern;
  int idx = -1;
  uint32_t nextAt = 0;
  bool on = false;
} led;

// ---------------------------------------------------------------- morse

static const char MORSE_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static const char *const MORSE_CODES[] = {
    ".-",    "-...",  "-.-.",  "-..",   ".",     "..-.",  "--.",   "....",  "..",
    ".---",  "-.-",   ".-..",  "--",    "-.",    "---",   ".--.",  "--.-",  ".-.",
    "...",   "-",     "..-",   "...-",  ".--",   "-..-",  "-.--",  "--..",  "-----",
    ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};

static char decodeMorse(const String &sym) {
  for (size_t i = 0; i < sizeof(MORSE_CHARS) - 1; i++)
    if (sym == MORSE_CODES[i]) return MORSE_CHARS[i];
  return '?';
}

static const char *encodeMorse(char c) {
  c = toupper(c);
  for (size_t i = 0; i < sizeof(MORSE_CHARS) - 1; i++)
    if (MORSE_CHARS[i] == c) return MORSE_CODES[i];
  return nullptr;
}

// Blink a received message on the white LED, non-blocking.
void ledPlay(const String &text) {
  String p;
  for (char c : text) {
    if (c == ' ') {
      p += '/';
      continue;
    }
    const char *m = encodeMorse(c);
    if (!m) continue;
    p += m;
    p += ' ';
  }
  led.pattern = p;
  led.idx = 0;
  led.on = false;
  led.nextAt = millis() + 500;
}

void ledFlash(int count) {
  led.pattern = "";
  for (int i = 0; i < count; i++) led.pattern += ". ";
  led.idx = 0;
  led.on = false;
  led.nextAt = millis();
}

static void ledUpdate() {
  if (led.idx >= 0 && millis() >= led.nextAt) {
    if (led.on) { // element done, one unit of silence
      led.on = false;
      led.nextAt = millis() + LED_UNIT_MS;
      led.idx++;
    } else if (led.idx >= (int)led.pattern.length()) {
      led.idx = -1;
    } else {
      char c = led.pattern[led.idx];
      if (c == '.' || c == '-') {
        led.on = true;
        led.nextAt = millis() + (c == '.' ? 1 : 3) * LED_UNIT_MS;
      } else { // letter gap (3u) or word gap (7u), 1u already elapsed
        led.nextAt = millis() + (c == ' ' ? 2 : 6) * LED_UNIT_MS;
        led.idx++;
      }
    }
  }
  bool sosBlink = sosActive && (millis() / 100) % 2;
  digitalWrite(LED_PIN, sosBlink || (key.down && !key.swallow) || led.on);
}

// ---------------------------------------------------------------- alerts

void alertSos(uint16_t from) {
  sosActive = true;
  sosFrom = from;
  displayWake();
  blePublishAlert("sos", from, "SOS");
}

void sosAcknowledge() {
  if (!sosActive) return;
  sosActive = false;
  showBanner("SOS acknowledged", 2000);
}

bool sosAlarm(uint16_t *from) {
  if (from) *from = sosFrom;
  return sosActive;
}

static void leashAlert(const char *kind, const Peer &p, const String &text) {
  showBanner(text);
  ledFlash(3);
  blePublishAlert(kind, p.id, text);
  Serial.printf("Leash: %s\n", text.c_str());
}

static void leashUpdate() {
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck < 2000) return;
  lastCheck = now;
  for (int i = 0; i < numPeers; i++) {
    Peer &p = peers[i];
    if (settings.leashM && gps.fix && p.fix && now - p.lastHeard < STALE_MS) {
      double d = distM(gps.lat, gps.lon, p.lat, p.lon);
      if (d > settings.leashM && !p.farAlerted) {
        p.farAlerted = true;
        leashAlert("far", p, idHex(p.id) + " far " + fmtDist(d));
      } else if (d < settings.leashM * 0.9) p.farAlerted = false;
    }
    if (settings.leashMin && !p.silentAlerted && now - p.lastHeard > settings.leashMin * 60000UL) {
      p.silentAlerted = true;
      leashAlert("silent", p, idHex(p.id) + " silent " + fmtAge(now - p.lastHeard));
    }
  }
}

// ---------------------------------------------------------------- battery

static int adcEnableLevel = HIGH;

static int readBatteryMv(int level) {
  digitalWrite(ADC_CTRL_PIN, level);
  delay(5);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(VBAT_PIN);
  digitalWrite(ADC_CTRL_PIN, !level);
  return sum / 8 * 490 / 100; // 390k/100k divider
}

static int batteryPercent(int mv) {
  static const int CURVE[][2] = {{4200, 100}, {4100, 90}, {4000, 78}, {3900, 65}, {3800, 50},
                                 {3700, 32},  {3600, 15}, {3500, 6},  {3300, 0}};
  if (mv >= CURVE[0][0]) return 100;
  for (int i = 1; i < 9; i++)
    if (mv >= CURVE[i][0])
      return CURVE[i][1] + (mv - CURVE[i][0]) * (CURVE[i - 1][1] - CURVE[i][1]) / (CURVE[i - 1][0] - CURVE[i][0]);
  return 0;
}

static void batteryBegin() {
  pinMode(ADC_CTRL_PIN, OUTPUT);
  // V3.0 and V3.1+ disagree on ADC_Ctrl polarity; the disabled divider reads ~0
  int high = readBatteryMv(HIGH), low = readBatteryMv(LOW);
  adcEnableLevel = high >= low ? HIGH : LOW;
  Serial.printf("Battery: ADC_Ctrl HIGH=%dmV LOW=%dmV -> enable %s\n", high, low, adcEnableLevel ? "HIGH" : "LOW");
}

static void batteryUpdate() {
  static uint32_t lastRead = 0;
  if (lastRead && millis() - lastRead < 10000) return;
  lastRead = millis();
  int mv = readBatteryMv(adcEnableLevel);
  batteryMv = batteryMv ? (batteryMv * 3 + mv) / 4 : mv;
  batteryPct = batteryMv < 2500 ? -1 : batteryPercent(batteryMv); // < 2.5 V: no battery fitted
}

// ---------------------------------------------------------------- button

static void submitDraft() {
  String t = key.text;
  t.trim();
  key.text = "";
  if (!t.length()) return;
  if (t == "SOS") sendText("SOS", true);
  else if (t == "MARK") showBanner(addWaypoint("") ? "Marked " + String(waypoints[numWaypoints - 1].name) : "No fix to mark");
  else sendText(t, false);
}

static void keyerUpdate() {
  uint32_t now = millis();
  bool down = digitalRead(BUTTON_PIN) == LOW;
  bool drafting = key.text.length() || key.symbols.length();

  if (down != key.down && now - key.lastChange > 20) {
    key.lastChange = now;
    key.down = down;
    if (down) {
      key.swallow = false;
      if (displayAsleep()) key.swallow = true;
      else if (sosActive) {
        sosAcknowledge();
        key.swallow = true;
      }
      displayWake();
      if (!key.swallow && key.text.length() && !key.symbols.length() && !key.text.endsWith(" ") &&
          now - key.releaseAt > WORD_GAP_MS)
        key.text += ' ';
      key.pressAt = now;
    } else {
      key.releaseAt = now;
      holdHint = "";
      uint32_t d = now - key.pressAt;
      if (key.swallow) {
      } else if (d < DASH_MS) key.symbols += '.';
      else if (d < PAGE_HOLD_MS) key.symbols += '-';
      else if (d < BT_HOLD_MS) {
        if (drafting) key.text = key.symbols = "";
        else page = (page + 1) % NUM_PAGES;
      } else if (d < FORGET_HOLD_MS) {
        settings.bleOn = !bleIsOn();
        bleSetEnabled(settings.bleOn);
        saveSettings();
        showBanner(settings.bleOn ? "Bluetooth ON" : "Bluetooth OFF", 2000);
      } else {
        bleForgetBonds();
        showBanner("Phones forgotten", 2000);
      }
    }
  }

  if (key.down && !key.swallow) {
    uint32_t d = now - key.pressAt;
    if (d >= FORGET_HOLD_MS) holdHint = "release: forget phones";
    else if (d >= BT_HOLD_MS) holdHint = bleIsOn() ? "release: BT OFF" : "release: BT ON";
    else if (d >= PAGE_HOLD_MS) holdHint = drafting ? "release: cancel" : "release: next page";
    else holdHint = "";
  }

  if (!key.down && key.symbols.length() && now - key.releaseAt > LETTER_GAP_MS) {
    key.text += decodeMorse(key.symbols);
    key.symbols = "";
  }

  if (!key.down && !key.symbols.length() && key.text.length() &&
      (now - key.releaseAt > SEND_GAP_MS || key.text.length() >= MAX_TEXT))
    submitDraft();

  bool keying = key.down && !key.swallow && now - key.pressAt < PAGE_HOLD_MS;
  draftLine = (key.text.length() || key.symbols.length() || keying) ? "> " + key.text + key.symbols + (keying ? "_" : "")
                                                                     : "";
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(300);

  uint64_t mac = ESP.getEfuseMac();
  myId = (uint16_t)(((mac >> 32) & 0xFF) << 8 | ((mac >> 40) & 0xFF)); // last two MAC bytes
  randomSeed(esp_random());
  Serial.printf("\nLoRa buddy tracker, id %04X\n", myId);

  storageBegin();
  displayBegin();
  gpsBegin();
  radioBegin();
  batteryBegin();
  if (settings.bleOn) bleSetEnabled(true);
}

void loop() {
  gpsUpdate();
  radioUpdate();
  scanUpdate();
  bleUpdate();
  keyerUpdate();
  ledUpdate();
  leashUpdate();
  batteryUpdate();
  trackUpdate();
  displayUpdate();
}
