/*
  Wemos D1 mini (ESP8266) + TM1637 4-digit (0.56")
  - TM1637: D3 (CLK) + D4 (DIO)
  - Buttons: D1 (BTN1) + D2 (BTN2) to GND, INPUT_PULLUP

  Functies:
  - Tijd (24h) via NTP + automatische zomer/wintertijd (Europe/Amsterdam)
  - BTN1 kort: helderder
  - BTN2 kort: minder fel
  - BTN1+BTN2 tegelijk: display aan/uit
  - BTN1 lang (~3s): force NTP sync → SYNC → OK (2s) / Err / noWi
  - BTN2 lang (~3s): reconnect WiFi → WIFI / noWi (geen OK bij succes)
  - 1x per nacht (03:00) stille NTP sync (geen display output)
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <time.h>
#include <TM1637Display.h>

// ---------- Hardware ----------
static const uint8_t PIN_CLK  = D3;
static const uint8_t PIN_DIO  = D4;
static const uint8_t PIN_BTN1 = D1;
static const uint8_t PIN_BTN2 = D2;

TM1637Display display(PIN_CLK, PIN_DIO);

// ---------- WiFi ----------
const char* WIFI_SSID = "yournetworkname";
const char* WIFI_PASS = "yournetworkpasswd";

// ---------- Tijd / TZ ----------
static const char* TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// ---------- Display / UI ----------
uint8_t brightness = 4;      // 0..7
bool displayOn = true;
bool colonOn = true;

static const unsigned long DISPLAY_TICK_MS = 250;
static const unsigned long COLON_TOGGLE_MS = 1000;
static const unsigned long DEBOUNCE_MS  = 35;
static const unsigned long LONGPRESS_MS = 3000;

// ---------- Nightly NTP ----------
static int lastNtpSyncDay = -1;          // tm_yday van laatste auto-sync
static const int AUTO_NTP_HOUR = 3;      // 03:00 's nachts

// ---------- Button state ----------
struct Button {
  uint8_t pin;
  bool stableState;          // true = pressed (active low), false = released
  bool lastRead;
  unsigned long lastChangeMs;
  unsigned long pressStartMs;
  bool longFired;
  bool shortEligible;
};

Button b1{PIN_BTN1, false, false, 0, 0, false, false};
Button b2{PIN_BTN2, false, false, 0, 0, false, false};

bool comboArmed = true;      // voorkomt herhaald togglen zolang beide ingedrukt blijven

// ---------- Helpers ----------
static inline bool readPressed(uint8_t pin) { return digitalRead(pin) == LOW; }

void setDisplayState(bool on) {
  displayOn = on;
  if (!displayOn) display.clear();
  else display.setBrightness(brightness, true);
}

void applyBrightness() {
  if (brightness > 7) brightness = 7;
  display.setBrightness(brightness, displayOn);
}

// ---------- WiFi / Time ----------
bool connectWiFi(unsigned long timeoutMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
    yield();
  }
  return WiFi.status() == WL_CONNECTED;
}

bool reconnectWiFi() {
  WiFi.disconnect(true);
  delay(200);
  return connectWiFi(15000);
}

bool waitForValidTime(unsigned long timeoutMs = 8000) {
  unsigned long start = millis();
  time_t now;
  while (millis() - start < timeoutMs) {
    time(&now);
    if (now > 1577836800UL) return true; // > 2020-01-01
    delay(100);
    yield();
  }
  return false;
}

void setupTime() {
  setenv("TZ", TZ_INFO, 1);
  tzset();
  configTime(TZ_INFO, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
}

// ---------- 7-seg glyphs ----------
static const uint8_t GLYPH_BLANK = 0x00;
static const uint8_t GLYPH_DASH  = 0x40; // middle segment only

static const uint8_t GLYPH_O = 0x3F; // looks like '0'
static const uint8_t GLYPH_K = 0x75; // approximation
static const uint8_t GLYPH_E = 0x79;
static const uint8_t GLYPH_r = 0x50;
static const uint8_t GLYPH_n = 0x54;
static const uint8_t GLYPH_o = 0x5C;
static const uint8_t GLYPH_W = 0x3E;
static const uint8_t GLYPH_I = 0x06;
static const uint8_t GLYPH_F = 0x71;
static const uint8_t GLYPH_S = 0x6D;
static const uint8_t GLYPH_Y = 0x6E;
static const uint8_t GLYPH_C = 0x39;

void showTextMs(uint8_t a, uint8_t b, uint8_t c, uint8_t d, unsigned long ms) {
  if (!displayOn) return;
  uint8_t segs[4] = {a, b, c, d};
  display.setSegments(segs);
  unsigned long t = millis();
  while (millis() - t < ms) {
    delay(10);
    yield();
  }
}

void showOK()   { showTextMs(GLYPH_BLANK, GLYPH_O, GLYPH_K, GLYPH_BLANK, 2000); }
void showErr()  { showTextMs(GLYPH_E, GLYPH_r, GLYPH_r, GLYPH_BLANK, 1200); }
void showNoWi() { showTextMs(GLYPH_n, GLYPH_o, GLYPH_W, GLYPH_I, 1200); }
void showWifi() { showTextMs(GLYPH_W, GLYPH_I, GLYPH_F, GLYPH_I, 600); }

void showSyncAnimation() {
  if (!displayOn) return;
  const uint8_t frames[][4] = {
    {GLYPH_S, GLYPH_Y, GLYPH_n, GLYPH_C},
    {GLYPH_Y, GLYPH_n, GLYPH_C, GLYPH_BLANK},
    {GLYPH_Y, GLYPH_n, GLYPH_C, GLYPH_DASH},
    {GLYPH_n, GLYPH_C, GLYPH_BLANK, GLYPH_BLANK},
  };
  for (uint8_t i = 0; i < 8; i++) {
    display.setSegments(frames[i % 4]);
    delay(200);
    yield();
  }
}

void forceNtpSync() {
  if (WiFi.status() != WL_CONNECTED) {
    showNoWi();
    return;
  }
  showSyncAnimation();
  setupTime();
  if (waitForValidTime(8000)) showOK();
  else showErr();
}

// ---------- Nightly NTP (silent) ----------
void nightlyNtpSyncIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return;

  time_t now;
  time(&now);
  if (now <= 1577836800UL) return; // tijd nog niet valide -> skip

  struct tm t;
  localtime_r(&now, &t);

  if (t.tm_hour != AUTO_NTP_HOUR) return;
  if (t.tm_yday == lastNtpSyncDay) return;

  // Silent sync
  setupTime();
  if (waitForValidTime(8000)) {
    lastNtpSyncDay = t.tm_yday;
  }
}

// ---------- Buttons ----------
void updateButton(Button &b, unsigned long nowMs) {
  bool pressed = readPressed(b.pin);

  if (pressed != b.lastRead) {
    b.lastRead = pressed;
    b.lastChangeMs = nowMs;
  }

  if (nowMs - b.lastChangeMs >= DEBOUNCE_MS && pressed != b.stableState) {
    b.stableState = pressed;
    if (pressed) {
      b.pressStartMs = nowMs;
      b.longFired = false;
      b.shortEligible = true;
    }
  }

  if (b.stableState && !b.longFired && nowMs - b.pressStartMs >= LONGPRESS_MS) {
    b.longFired = true;
    b.shortEligible = false;
  }
}

bool bothPressed() { return b1.stableState && b2.stableState; }

void handleButtons() {
  unsigned long now = millis();

  // Combo: display toggle
  if (bothPressed()) {
    if (comboArmed) {
      comboArmed = false;
      setDisplayState(!displayOn);

      b1.shortEligible = b2.shortEligible = false;
      b1.longFired = b2.longFired = false;
      b1.pressStartMs = b2.pressStartMs = now;
    }
  } else {
    comboArmed = true;
  }

  // Long presses
  if (!bothPressed()) {
    if (b1.longFired) {
      b1.longFired = false;
      forceNtpSync();
      b1.pressStartMs = millis(); // FIX: voorkom dubbel triggeren
    }

    if (b2.longFired) {
      b2.longFired = false;

      showWifi();
      if (!reconnectWiFi()) {
        showNoWi();
      } else {
        // Geen OK bij WiFi succes (voorkomt verwarring/dubbele OK)
        setupTime(); // NTP kan daarna stil syncen
      }

      b2.pressStartMs = millis(); // FIX: voorkom dubbel triggeren
    }
  }

  // Short presses on release
  if (!b1.stableState && b1.shortEligible) {
    b1.shortEligible = false;
    if (brightness < 7) brightness++;
    applyBrightness();
  }

  if (!b2.stableState && b2.shortEligible) {
    b2.shortEligible = false;
    if (brightness > 0) brightness--;
    applyBrightness();
  }
}

// ---------- Display time ----------
void updateDisplayTime() {
  if (!displayOn) return;

  time_t now;
  time(&now);
  struct tm t;
  localtime_r(&now, &t);

  uint8_t dots = colonOn ? 0b01000000 : 0;
  display.showNumberDecEx(t.tm_hour * 100 + t.tm_min, dots, true);
}

// ---------- Main ----------
void setup() {
  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);

  display.setBrightness(brightness, true);
  display.clear();

  bool wifiOk = connectWiFi(15000);
  setupTime();
  bool timeOk = waitForValidTime(8000);

  if (displayOn) {
    if (wifiOk && timeOk) showOK();
    else if (!wifiOk)     showNoWi();
    else                  showErr();
  }
}

void loop() {
  unsigned long now = millis();

  updateButton(b1, now);
  updateButton(b2, now);
  handleButtons();

  // Nightly silent NTP
  nightlyNtpSyncIfNeeded();

  // Colon blink
  static unsigned long lastColon = 0;
  if (now - lastColon >= COLON_TOGGLE_MS) {
    lastColon = now;
    colonOn = !colonOn;
  }

  // Display update
  static unsigned long lastDisp = 0;
  if (now - lastDisp >= DISPLAY_TICK_MS) {
    lastDisp = now;
    updateDisplayTime();
  }

  // (optioneel) eenvoudige auto-reconnect zou hier kunnen, maar nu alleen via BTN2
}
