/*
 * ESP CTF Game – Node Firmware (v2)
 * Compatible with ESP32 (empfohlen) and ESP8266.
 *
 * Each node:
 *   - Connects to the master's WiFi AP
 *   - Registers itself by MAC address and receives a stable node ID
 *   - Monitors a button (with debounce), presses are delivered with ACK + retries
 *   - Drives an RGB LED (NeoPixel WS2812B-8 bar or 3-pin RGB)
 *   - Executes animations (solid, blink, pulse, flash, bar, split)
 *   - Sends a STATUS every 2 s (LED-Seq, signal, firmware version, reset reason)
 *   - Can be updated via Arduino IDE (ArduinoOTA) or by the master (HTTP-OTA, ESP32)
 */

// config.h MUSS zuerst kommen – definiert LED_NEOPIXEL, Pins, etc.
#include "config.h"

// Fallbacks fuer aeltere config.h-Kopien (neue Optionen ab v2)
#ifndef OTA_PASSWORD
  #define OTA_PASSWORD ""        // Passwort fuer Arduino-IDE-OTA ("" = keins)
#endif
#ifndef USE_WATCHDOG
  #define USE_WATCHDOG 1         // 1 = Watchdog startet die Node neu, falls sie haengt
#endif
#ifndef WDT_TIMEOUT_S
  #define WDT_TIMEOUT_S 30
#endif
#ifndef WIFI_TIMEOUT_MS
  #define WIFI_TIMEOUT_MS 15000
#endif
#ifndef NEO_COUNT
  #define NEO_COUNT 8
#endif
#ifndef BUTTON_DEBOUNCE_MS
  #define BUTTON_DEBOUNCE_MS 50
#endif

#include "protocol.h"

#ifdef ESP32
  #include <WiFi.h>
  #include <Update.h>
  #include "esp_ota_ops.h"
  #include "esp_mac.h"
  #include "esp_system.h"
  #include "esp_task_wdt.h"
  // Vorab-Deklaration mit C-Linkage – MUSS vor den automatisch erzeugten
  // Prototypen der Arduino-IDE stehen (sonst "conflicting declaration").
  extern "C" bool verifyRollbackLater();
#else
  #include <ESP8266WiFi.h>
#endif
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#ifdef LED_NEOPIXEL
  #include <Adafruit_NeoPixel.h>
#endif

// Zeiten (Teil des Protokolls – nur zusammen mit dem Master aendern)
#define STATUS_INTERVAL_MS     2000UL   // STATUS/Keepalive an den Master
#define MASTER_LOST_MS        20000UL   // so lange kein PONG -> ID verwerfen, neu registrieren (Master: Timeout 15 s)
#define REGISTER_INTERVAL_MS   1000UL   // Registrierung wiederholen (+ Zufall 0..300 ms)
#define WIFI_OVERLAY_MS        3000UL   // WLAN so lange weg -> gelb blinken
#define WIFI_RECONNECT_MS     20000UL   // WLAN so lange weg -> WiFi.reconnect() (dann alle 20 s)
#define ROLLBACK_OK_MS        90000UL   // spaetestens dann gilt neue Firmware als gut
#define OTA_ERROR_SHOW_MS      2000UL   // rot blinken nach OTA-Fehler
// HTTP-OTA (ESP32)
#define OTA_CONNECT_TIMEOUT_MS 5000
#define OTA_HEADER_TIMEOUT_MS 15000UL   // Statuszeile + Header (Master bedient nur einen Web-Client gleichzeitig)
#define OTA_IDLE_TIMEOUT_MS   10000UL   // so lange keine Daten -> Abbruch
#define OTA_TOTAL_TIMEOUT_MS 120000UL   // Gesamtdauer Download
#define OTA_HDR_LEN           0x140     // so viele Byte werden VOR Update.begin() geprueft

static_assert(sizeof(Packet) == 8, "Packet muss 8 Byte gross sein");
static_assert(CTF_DESC_OFFSET + sizeof(CtfFwDesc) <= OTA_HDR_LEN, "OTA_HDR_LEN zu klein");

// ─────────────────────────────────────────────────────────────
// Firmware-Kennung + Rollback (ESP32)
// ─────────────────────────────────────────────────────────────
#ifdef ESP32
// Landet im .bin bei Offset 0x120. Master und Node pruefen damit Firmware-Dateien
// (richtige Rolle, passendes Protokoll) bevor sie geflasht werden.
extern "C" const CtfFwDesc ctf_fw_desc __attribute__((section(".rodata_custom_desc"), used)) =
  { CTF_DESC_MAGIC, CTF_ROLE_NODE, PROTO_VERSION, CTF_FW_MAJOR, CTF_FW_MINOR, CTF_FW_PATCH, {0, 0, 0}, CTF_FW_VERSION };

// Neue Firmware gilt erst als gut, wenn sie sich beim Master registriert hat (oder
// 90 s laeuft). Stuerzt sie vorher ab, bootet beim naechsten Start die alte Firmware.
extern "C" bool verifyRollbackLater() { return true; }

#ifdef CONFIG_IDF_FIRMWARE_CHIP_ID
  #define OTA_CHIP_ID CONFIG_IDF_FIRMWARE_CHIP_ID
#else
  #define OTA_CHIP_ID 0
#endif
#endif

bool fwValid = false;   // Firmware bereits als gueltig markiert?

void markFwValid(const char *why) {
  if (fwValid) return;
  fwValid = true;
#ifdef ESP32
  esp_ota_mark_app_valid_cancel_rollback();   // harmlos, falls kein Rollback ansteht
#endif
  Serial.printf("[FW] Firmware %s bestaetigt (%s)\n", CTF_FW_VERSION, why);
}

// Watchdog fuettern (nur noetig in langen Schleifen: WLAN-Wartezeit, OTA)
void wdtFeed() {
#if defined(ESP32) && USE_WATCHDOG
  feedLoopWDT();
#else
  yield();
#endif
}

// ─────────────────────────────────────────────────────────────
// LED
// ─────────────────────────────────────────────────────────────

// 24-bit RGB color table indexed by COL_*
static const uint32_t COLORS[] = {
  0x000000,  // OFF
  0xFF0000,  // RED
  0x0000FF,  // BLUE
  0x00FF00,  // GREEN
  0xFFFF00,  // YELLOW
  0xFFFFFF,  // WHITE
  0x800080,  // PURPLE
  0x00FFFF,  // CYAN
  0xFF8000,  // ORANGE
};
static_assert(sizeof(COLORS) / sizeof(COLORS[0]) == COL_COUNT, "COLORS[] passt nicht zu COL_COUNT");

uint32_t colorOf(uint8_t idx) { return COLORS[(idx < COL_COUNT) ? idx : COL_OFF]; }

#ifdef LED_NEOPIXEL
  Adafruit_NeoPixel strip(NEO_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
#endif

void applyRGB(uint8_t r, uint8_t g, uint8_t b) {
#ifdef LED_NEOPIXEL
  for (int i = 0; i < NEO_COUNT; i++) strip.setPixelColor(i, r, g, b);
  strip.show();
#elif defined(LED_RGB_CATHODE)
  analogWrite(LED_R_PIN, r);
  analogWrite(LED_G_PIN, g);
  analogWrite(LED_B_PIN, b);
#elif defined(LED_RGB_ANODE)
  analogWrite(LED_R_PIN, 255 - r);
  analogWrite(LED_G_PIN, 255 - g);
  analogWrite(LED_B_PIN, 255 - b);
#endif
}

void applyColor(uint32_t rgb) {
  applyRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// ─────────────────────────────────────────────────────────────
// Animation state (normale Anzeige, vom Master gesteuert)
// ─────────────────────────────────────────────────────────────

struct LedState {
  uint8_t  colorIdx;    // COL_* index
  uint8_t  anim;        // ANIM_*
  uint32_t lastChange;
  uint8_t  phase;       // toggle/step counter (Blinken: 0 = AN)
  bool     flashDone;
  uint8_t  barCount;    // for ANIM_BAR / ANIM_SPLIT: number of leading LEDs
  uint8_t  barBg;       // for ANIM_BAR: background color index (trailing LEDs)
  bool     dirty;       // render needed – prevents calling strip.show() every loop
};
LedState led = {COL_OFF, ANIM_SOLID, 0, 0, false, 0, COL_OFF, true};

void setLed(uint8_t colorIdx, uint8_t anim) {
  if (colorIdx >= COL_COUNT) colorIdx = COL_OFF;
  if (anim >= ANIM_COUNT)    anim = ANIM_SOLID;
  // Unveraendert -> nichts tun (kein Neustart der Animation, kein strip.show()).
  // FLASH ist ein einmaliges Ereignis und startet immer neu.
  if (colorIdx == led.colorIdx && anim == led.anim && anim != ANIM_FLASH) return;
  led.colorIdx   = colorIdx;
  led.anim       = anim;
  led.lastChange = millis();
  led.phase      = 0;       // Blinken startet in der AN-Phase
  led.flashDone  = false;
  led.dirty      = true;
}

// ANIM_BAR / ANIM_SPLIT: 'count' fuehrende LEDs in Farbe A, Rest in Farbe B
void setBar(uint8_t anim, uint8_t colorA, uint8_t count, uint8_t colorB) {
  led.anim       = anim;
  led.colorIdx   = (colorA < COL_COUNT) ? colorA : COL_OFF;
  led.barCount   = (count <= NEO_COUNT) ? count : NEO_COUNT;
  led.barBg      = (colorB < COL_COUNT) ? colorB : COL_OFF;
  led.lastChange = millis();
  led.phase      = 0;
  led.flashDone  = false;
  led.dirty      = true;
}

// strip.show() deaktiviert Interrupts fuer ~300µs pro Aufruf. Ohne Throttle
// wird es tausende Male/s aufgerufen -> ESP32 WiFi-Stack kann keine Pakete
// empfangen -> Nodes "haengen" oder verlieren die Verbindung.
// Jetzt wird nur noch gerendert wenn sich der Zustand wirklich aendert.
void updateLed() {
  uint32_t now   = millis();
  uint32_t color = colorOf(led.colorIdx);

  switch (led.anim) {
    case ANIM_BLINK_SLOW:
      if (now - led.lastChange >= 500) {
        led.phase ^= 1; led.lastChange = now; led.dirty = true;
      }
      if (!led.dirty) return;
      applyColor(led.phase ? 0 : color);   // Phase 0 = AN -> sofort sichtbar
      led.dirty = false;
      break;

    case ANIM_BLINK_FAST:
      if (now - led.lastChange >= 125) {
        led.phase ^= 1; led.lastChange = now; led.dirty = true;
      }
      if (!led.dirty) return;
      applyColor(led.phase ? 0 : color);
      led.dirty = false;
      break;

    case ANIM_PULSE: {
      // 30fps – haeufiger ist fuer das Auge nicht sichtbar aber kostet WiFi-Zeit
      if (now - led.lastChange < 33) return;
      led.lastChange = now;
      led.dirty = false;
      uint32_t t   = (now % 2000);
      uint8_t  val = (t < 1000) ? (t / 4) : (255 - ((t - 1000) / 4));
      uint8_t  r   = ((color >> 16) & 0xFF) * val / 255;
      uint8_t  g   = ((color >>  8) & 0xFF) * val / 255;
      uint8_t  b   = ( color        & 0xFF) * val / 255;
      applyRGB(r, g, b);
      break;
    }

    case ANIM_FLASH:
      if (!led.flashDone) {
        if (led.phase == 0) {
          if (led.dirty) { applyColor(0xFFFFFF); led.dirty = false; }
          if (now - led.lastChange >= 80) { led.phase = 1; led.lastChange = now; led.dirty = true; }
        } else if (led.phase == 1) {
          if (led.dirty) { applyColor(0); led.dirty = false; }
          if (now - led.lastChange >= 60) { led.phase = 2; led.lastChange = now; led.dirty = true; }
        } else {
          led.flashDone = true; led.dirty = true;
        }
      } else {
        if (!led.dirty) return;
        applyColor(color);
        led.dirty = false;
      }
      break;

    case ANIM_BAR:
      if (!led.dirty) return;
#ifdef LED_NEOPIXEL
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t c = (i < led.barCount) ? color : colorOf(led.barBg);
        strip.setPixelColor(i, c);
      }
      strip.show();
#else
      applyColor((led.barCount > 0) ? color : colorOf(led.barBg));
#endif
      led.dirty = false;
      break;

    case ANIM_SPLIT:
      if (!led.dirty) return;
#ifdef LED_NEOPIXEL
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t c = (i < led.barCount) ? color : colorOf(led.barBg);
        strip.setPixelColor(i, c);
      }
      strip.show();
#else
      applyColor((led.barCount >= 4) ? color : colorOf(led.barBg));
#endif
      led.dirty = false;
      break;

    case ANIM_SOLID:
    default:
      if (!led.dirty) return;
      applyColor(color);
      led.dirty = false;
      break;
  }
}

// ─────────────────────────────────────────────────────────────
// Lokale Overlays (nicht seq-verfolgt): ueberdecken die normale Anzeige.
// Endet das Overlay, wird die normale Anzeige wiederhergestellt (dirty).
// ─────────────────────────────────────────────────────────────
#define OV_NONE     0
#define OV_IDENTIFY 1   // weiss schnell blinken ("Finden")
#define OV_ERROR    2   // rot schnell blinken (OTA-Fehler)
#define OV_WIFI     3   // gelb blinken (WLAN weg)
#define OV_REDRAW   0xFF

uint8_t  ovMode      = OV_NONE;   // Timer-Overlay: OV_IDENTIFY oder OV_ERROR
uint32_t ovUntil     = 0;
bool     wifiOverlay = false;     // WLAN laenger als 3 s weg
uint8_t  ovShown     = OV_NONE;   // zuletzt dargestellt (OV_REDRAW = neu zeichnen)
uint32_t ovSince     = 0;         // seit wann ovShown aktiv (Blinken startet AN)
int8_t   ovLastOn    = -1;        // zuletzt gerendert: 1 = an, 0 = aus

void startOverlay(uint8_t mode, uint32_t ms) {
  ovMode  = mode;
  ovUntil = millis() + ms;
}

bool identifyActive() {
  return ovMode == OV_IDENTIFY && (int32_t)(millis() - ovUntil) < 0;
}

// Nach direktem Zeichnen (OTA-Balken) alles neu zeichnen
void ledForceRedraw() { ovShown = OV_REDRAW; }

void renderLed() {
  uint32_t now = millis();
  if (ovMode != OV_NONE && (int32_t)(now - ovUntil) >= 0) ovMode = OV_NONE;   // abgelaufen
  uint8_t ov = wifiOverlay ? OV_WIFI : ovMode;
  if (ov != ovShown) {
    ovShown  = ov;
    ovSince  = now;
    ovLastOn = -1;
    if (ov == OV_NONE) led.dirty = true;   // normale Anzeige wiederherstellen
  }
  if (ov == OV_NONE) { updateLed(); return; }

  uint32_t col, half;
  if (ov == OV_WIFI)       { col = COLORS[COL_YELLOW]; half = 300; }
  else if (ov == OV_ERROR) { col = COLORS[COL_RED];    half = 125; }
  else                     { col = COLORS[COL_WHITE];  half = 125; }
  int8_t on = (((now - ovSince) / half) % 2 == 0) ? 1 : 0;
  if (on != ovLastOn) { ovLastOn = on; applyColor(on ? col : 0); }
}

// Lila Fortschrittsbalken (OTA). Zeichnet nur bei Aenderung.
int8_t barLit = -1;

void drawProgressBar(uint32_t done, uint32_t total) {
  if (total == 0) total = 1;
  if (done > total) done = total;
  int8_t lit = (int8_t)((uint64_t)done * NEO_COUNT / total);
  if (lit == barLit) return;
  barLit = lit;
#ifdef LED_NEOPIXEL
  for (int i = 0; i < NEO_COUNT; i++)
    strip.setPixelColor(i, i < lit ? 0x800080 : 0x300030);   // fertig = hell, Rest = schwach lila
  strip.show();
#else
  applyColor(0x800080);
#endif
  ledForceRedraw();   // danach normale Anzeige/Overlay komplett neu zeichnen
}

// ─────────────────────────────────────────────────────────────
// Button
// ─────────────────────────────────────────────────────────────

bool     lastRaw       = HIGH;
bool     lastDebounced = HIGH;
uint32_t lastChangeTs  = 0;

// Returns true on the falling edge (press)
bool buttonPressed() {
  bool raw = digitalRead(BUTTON_PIN);
  uint32_t now = millis();

  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeTs = now;
  }
  if (now - lastChangeTs >= BUTTON_DEBOUNCE_MS && raw != lastDebounced) {
    lastDebounced = raw;
    if (raw == LOW) return true;   // falling edge = press
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
// Network
// ─────────────────────────────────────────────────────────────

WiFiUDP   udp;
IPAddress masterIP;
uint8_t   myMac[6]       = {0};
uint8_t   resetReason    = 0;      // esp_reset_reason(), 0..15
uint8_t   myId           = 0;      // 0 = nicht registriert
uint8_t   ledApplied     = 0;      // zuletzt angewendete LED-Seq (0 = keine)
uint8_t   nodeFlags      = 0;      // NODE_FLAG_OTA_BUSY / NODE_FLAG_OTA_FAILED
uint32_t  nextRegisterAt = 0;      // naechste Registrierung (millis)
uint16_t  regAttempts    = 0;
uint32_t  lastStatus     = 0;
uint32_t  lastPong       = 0;
bool      wifiUp         = false;  // WLAN-Zustand aus Sicht von loop()
uint32_t  wifiLostSince  = 0;
uint32_t  lastReconnect  = 0;

// Tastendruck-Zustellung (Senden + Wiederholen bis PKT_BTN_ACK)
#define BTN_MAX_SENDS 6
// Wartezeit nach Sendung Nr. 1..6 (nach der 6. nur noch auf Quittung warten)
static const uint16_t BTN_GAPS[BTN_MAX_SENDS] = {30, 60, 120, 240, 400, 400};
uint8_t   pressSeq   = 0;          // 1..255, 0 = noch kein Druck in dieser Sitzung
uint8_t   btnNonce   = 1;          // 1..255, neu bei jeder Registrierung
bool      btnPending = false;
uint8_t   btnTries   = 0;          // bisherige Sendungen des offenen Drucks
uint32_t  btnSentAt  = 0;

// HTTP-OTA-Anforderung vom Master (wird in loop() ausgefuehrt)
bool      otaRequested = false;
uint32_t  otaAnnounced = 0;

void sendPkt(uint8_t type, uint8_t nid,
             uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
             uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  if (WiFi.status() != WL_CONNECTED) return;   // ohne WLAN nichts senden
  Packet p;
  p.type = type; p.nodeId = nid;
  p.data[0]=d0; p.data[1]=d1; p.data[2]=d2;
  p.data[3]=d3; p.data[4]=d4; p.data[5]=d5;
  udp.beginPacket(masterIP, UDP_PORT);
  udp.write((uint8_t*)&p, sizeof(p));
  udp.endPacket();
}

void sendStatus() {
  lastStatus = millis();
  if (myId == 0) return;
  uint8_t fl = nodeFlags & NODE_FLAG_MASK;
  if (identifyActive()) fl |= NODE_FLAG_IDENTIFY;
  sendPkt(PKT_STATUS, myId, ledApplied, (uint8_t)(int8_t)WiFi.RSSI(),
          CTF_FW_MAJOR, CTF_FW_MINOR, CTF_FW_PATCH, (uint8_t)(fl | (resetReason << 4)));
}

void sendOtaStatus(uint8_t st, uint8_t pct, uint8_t err) {
  sendPkt(PKT_OTA_STATUS, myId, st, pct, err);
}

// ID verwerfen und nach Zufallspause (0..maxJitterMs) neu registrieren
void dropRegistration(const char *why, uint32_t maxJitterMs) {
  if (myId != 0) Serial.printf("[NODE] %s – ID %u verworfen, registriere neu\n", why, myId);
  myId         = 0;
  btnPending   = false;
  otaRequested = false;
  regAttempts  = 0;
  nextRegisterAt = millis() + (uint32_t)random(0, (long)maxJitterMs + 1);
  setLed(COL_BLUE, ANIM_BLINK_SLOW);   // blau langsam = nicht registriert
}

// Unicast-Befehl pruefen: fuer uns? Fremde ID (nicht 0xFF) = Desync -> neu registrieren.
bool forMe(const Packet &p) {
  if (myId == 0) return false;               // nicht registriert: ignorieren
  if (p.nodeId == myId) return true;
  if (p.nodeId != 0xFF) {
    Serial.printf("[NODE] Paket 0x%02X fuer ID %u, eigene ID %u\n", p.type, p.nodeId, myId);
    dropRegistration("Desync", 300);
  }
  return false;
}

// LED-Befehl anwenden (Werte werden in setLed/setBar geprueft/begrenzt)
void applyLedCmd(const Packet &p) {
  if (p.type == PKT_SET_LED) {
    uint8_t anim = p.data[1];
    if (anim >= ANIM_BAR) anim = ANIM_SOLID;   // BAR/SPLIT nur per eigenem Paket, Unbekanntes -> SOLID
    setLed(p.data[0], anim);
  } else {
    setBar(p.type == PKT_SET_BAR ? ANIM_BAR : ANIM_SPLIT, p.data[0], p.data[1], p.data[2]);
  }
}

void handlePacket(const Packet &p) {
  uint32_t now = millis();
  switch (p.type) {
    case PKT_ACK: {
      // Nur annehmen, wenn Protokoll passt und die MAC unsere ist
      if (p.data[1] != PROTO_VERSION || memcmp(&p.data[2], &myMac[2], 4) != 0) return;
      uint8_t id = p.data[0];
      if (id == 0 || id == 0xFF) return;
      bool fresh = (myId == 0);
      myId        = id;
      ledApplied  = 0;                          // Master sendet den Soll-Zustand neu
      { uint8_t nn; do nn = (uint8_t)random(1, 256); while (nn == btnNonce); btnNonce = nn; }   // neue Tasten-Sitzung (immer anderer Wert)
      pressSeq    = 0;
      btnPending  = false;
      lastPong    = now;
      regAttempts = 0;
      if (fresh) {
        Serial.printf("[NODE] Registriert als Node %u\n", myId);
        setLed(COL_GREEN, ANIM_FLASH);
      }
      markFwValid("Registrierung");
      sendStatus();
      return;
    }

    case PKT_RESET:
      // nodeId 0xFF = alle, sonst nur unsere eigene ID
      if (p.nodeId != 0xFF && (myId == 0 || p.nodeId != myId)) return;
      dropRegistration("Reset vom Master", 1500);
      return;

    case PKT_SET_LED:
    case PKT_SET_BAR:
    case PKT_SET_SPLIT:
      if (!forMe(p)) return;
      if (p.data[5] != ledApplied) {            // neue Seq -> anwenden
        applyLedCmd(p);
        ledApplied = p.data[5];
      }
      sendStatus();                             // Quittung (auch bei Duplikat)
      return;

    case PKT_BTN_ACK:
      if (!forMe(p)) return;
      if (btnPending && p.data[0] == pressSeq && p.data[1] == btnNonce) {
        btnPending = false;
        if (btnTries > 1) Serial.printf("[NODE] Taste seq=%u quittiert nach %u Sendungen\n", pressSeq, btnTries);
      }
      return;

    case PKT_PONG:
      if (!forMe(p)) return;
      lastPong = now;
      return;

    case PKT_IDENTIFY: {
      if (!forMe(p)) return;
      uint8_t s = p.data[0];
      if (s == 0) s = 3;
      if (s > 10) s = 10;
      Serial.printf("[NODE] Identifizieren: %u s weiss blinken\n", s);
      startOverlay(OV_IDENTIFY, (uint32_t)s * 1000UL);
      sendStatus();
      return;
    }

    case PKT_OTA:
      if (!forMe(p)) return;
#ifdef ESP32
      if (!otaRequested) {
        otaRequested = true;
        otaAnnounced = (uint32_t)p.data[0] | ((uint32_t)p.data[1] << 8) |
                       ((uint32_t)p.data[2] << 16) | ((uint32_t)p.data[3] << 24);
      }
#else
      sendOtaStatus(OTA_ST_FAILED, 0, OTA_ERR_BUSY);   // ESP8266: kein HTTP-OTA
#endif
      return;

    case PKT_GAME_START:
      Serial.printf("[NODE] Game started, mode=%u\n", p.data[0]);
      return;

    case PKT_GAME_OVER:
      Serial.println("[NODE] Game over");
      return;

    default:
      return;
  }
}

void udpDiscard() {
#ifdef ESP32
  udp.clear();   // Rest des aktuellen Datagramms verwerfen (sonst blockiert parsePacket)
#endif           // ESP8266: parsePacket() verwirft den Rest selbst
}

void handleUDP() {
  // GESAMTEN Empfangspuffer leeren (nicht nur 1 Paket pro Loop) – sonst stauen
  // sich Pakete und LED-Befehle/ACKs kommen Sekunden zu spaet an.
  // Pakete mit falscher Groesse MUESSEN verworfen werden, sonst liefert
  // parsePacket() fuer immer 0 und die Node ist "taub".
  int n;
  uint8_t guard = 0;
  while ((n = udp.parsePacket()) > 0) {
    if (n != (int)sizeof(Packet)) { udpDiscard(); continue; }
    Packet p;
    if (udp.read((uint8_t*)&p, sizeof(p)) != (int)sizeof(p)) { udpDiscard(); continue; }
    if (udp.remoteIP() != masterIP) continue;   // nur Pakete vom Master
    handlePacket(p);
    if (++guard >= 32) break;                   // loop() nicht aushungern
  }
}

// Alle wartenden Pakete verwerfen (nach langem Blockieren sind sie veraltet)
void udpDiscardAll() {
  for (uint8_t i = 0; i < 64; i++) {
    if (udp.parsePacket() <= 0) break;
    udpDiscard();               // jedes angenommene Datagramm sofort verwerfen
  }
  udpDiscard();                 // sicherheitshalber (harmlos, wenn leer)
}

// ─────────────────────────────────────────────────────────────
// Registrierung / Status / Master-Verlust
// ─────────────────────────────────────────────────────────────

void registrationService() {
  if (myId != 0) return;
  uint32_t now = millis();
  if ((int32_t)(now - nextRegisterAt) < 0) return;
  nextRegisterAt = now + REGISTER_INTERVAL_MS + (uint32_t)random(0, 301);
  regAttempts++;
  sendPkt(PKT_REGISTER, PROTO_VERSION, myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  if (regAttempts == 1 || regAttempts % 10 == 0)
    Serial.printf("[NODE] Registriere bei %s ... (Versuch %u)\n", MASTER_IP, regAttempts);
}

void statusService() {
  if (myId == 0) return;
  uint32_t now = millis();
  if (now - lastStatus >= STATUS_INTERVAL_MS) sendStatus();
  if (myId != 0 && now - lastPong >= MASTER_LOST_MS) dropRegistration("Master antwortet nicht (20 s kein PONG)", 500);
}

// ─────────────────────────────────────────────────────────────
// Taster: einmal senden, dann ohne Blockieren wiederholen bis zur Quittung
// ─────────────────────────────────────────────────────────────

void onButtonPress() {
  if (myId == 0 || !wifiUp) {
    Serial.printf("[NODE] Taste erkannt, aber %s – nicht gesendet\n",
                  myId == 0 ? "nicht registriert" : "kein WLAN");
    return;
  }
  pressSeq   = (pressSeq >= 255) ? 1 : pressSeq + 1;   // 1..255, nie 0
  btnPending = true;                                    // ersetzt einen offenen Druck
  btnTries   = 1;
  btnSentAt  = millis();
  sendPkt(PKT_BUTTON, myId, pressSeq, btnNonce);
  Serial.printf("[NODE] Taste gedrueckt (id=%u seq=%u) -> gesendet\n", myId, pressSeq);
}

void buttonRetryService() {
  if (!btnPending) return;
  if (myId == 0 || !wifiUp || btnTries < 1 || btnTries > BTN_MAX_SENDS) { btnPending = false; return; }
  if (millis() - btnSentAt < BTN_GAPS[btnTries - 1]) return;
  if (btnTries >= BTN_MAX_SENDS) {
    btnPending = false;
    Serial.printf("[NODE] Taste seq=%u: keine Quittung vom Master – aufgegeben\n", pressSeq);
    return;
  }
  sendPkt(PKT_BUTTON, myId, pressSeq, btnNonce);
  btnTries++;
  btnSentAt = millis();
}

// ─────────────────────────────────────────────────────────────
// HTTP-OTA vom Master (ESP32): laedt http://<MASTER_IP>/fw/node.bin
// Blockiert waehrend des Downloads (Watchdog wird gefuettert).
// ─────────────────────────────────────────────────────────────
#ifdef ESP32
static uint8_t otaBuf[2048];

const char *otaErrText(uint8_t e) {
  switch (e) {
    case OTA_ERR_CONNECT:  return "keine Verbindung zum Master";
    case OTA_ERR_HTTP:     return "ungueltige HTTP-Antwort";
    case OTA_ERR_SIZE:     return "Dateigroesse ungueltig";
    case OTA_ERR_BEGIN:    return "Update.begin fehlgeschlagen";
    case OTA_ERR_DOWNLOAD: return "Download abgebrochen/Timeout";
    case OTA_ERR_WRITE:    return "Flash-Schreibfehler";
    case OTA_ERR_VERIFY:   return "Pruefung (MD5/Image) fehlgeschlagen";
    case OTA_ERR_BUSY:     return "nicht bereit";
    case OTA_ERR_IMAGE:    return "keine passende Node-Firmware";
    default:               return "unbekannt";
  }
}

// Eine Header-Zeile lesen (ohne \r\n, zu lange Zeilen werden abgeschnitten).
// Rueckgabe: Laenge, oder -1 bei Timeout (Deadline ab 'start') / Verbindungsfehler.
int otaReadLine(NetworkClient &c, char *buf, size_t sz, uint32_t start) {
  size_t n = 0;
  buf[0] = 0;
  for (;;) {
    if (millis() - start >= OTA_HEADER_TIMEOUT_MS) return -1;
    uint8_t ch;
    int r = c.read(&ch, 1);
    if (r < 0) return -1;
    if (r == 0) { wdtFeed(); delay(1); continue; }
    if (ch == '\n') { buf[n] = 0; return (int)n; }
    if (ch != '\r' && n + 1 < sz) buf[n++] = (char)ch;
  }
}

// Content-Length: nur Ziffern (max. 9), Leerzeichen davor/danach erlaubt
bool otaParseLen(const char *s, uint32_t *out) {
  while (*s == ' ' || *s == '\t') s++;
  uint32_t v = 0;
  int digits = 0;
  while (*s >= '0' && *s <= '9') {
    if (++digits > 9) return false;
    v = v * 10 + (uint32_t)(*s - '0');
    s++;
  }
  while (*s == ' ' || *s == '\t') s++;
  if (digits == 0 || *s != 0) return false;
  *out = v;
  return true;
}

// x-MD5: genau 32 Hex-Zeichen -> out (33 Byte, Kleinbuchstaben). Sonst out unveraendert.
bool otaParseMd5(const char *s, char *out) {
  while (*s == ' ' || *s == '\t') s++;
  char tmp[33];
  int n = 0;
  while (isxdigit((unsigned char)*s)) {
    if (n >= 32) return false;
    tmp[n++] = (char)tolower((unsigned char)*s);
    s++;
  }
  while (*s == ' ' || *s == '\t') s++;
  if (n != 32 || *s != 0) return false;
  memcpy(out, tmp, 32);
  out[32] = 0;
  return true;
}

uint32_t rdLE32(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Bis zu 'want' Byte lesen. Wartet hoechstens OTA_IDLE_TIMEOUT_MS auf Daten und nie
// ueber die Gesamt-Deadline (ab t0) hinaus. Rueckgabe: >0 gelesen, 0 = Timeout/Abbruch.
int otaRead(NetworkClient &c, uint8_t *dst, size_t want, uint32_t t0) {
  uint32_t idleStart = millis();
  for (;;) {
    wdtFeed();
    uint32_t now = millis();
    if (now - t0 >= OTA_TOTAL_TIMEOUT_MS) { Serial.println("[OTA] Gesamt-Timeout"); return 0; }
    if (now - idleStart >= OTA_IDLE_TIMEOUT_MS) { Serial.println("[OTA] Keine Daten (Timeout)"); return 0; }
    int r = c.read(dst, want);
    if (r > 0) return r;
    if (r < 0) { Serial.println("[OTA] Verbindung abgebrochen"); return 0; }
    delay(1);
  }
}

// Fortschritt: Balken + OTA_STATUS{PROGRESS} alle 10 %
void otaProgress(uint32_t done, uint32_t total, uint8_t &lastStep) {
  drawProgressBar(done, total);
  uint8_t pct = (uint8_t)((uint64_t)done * 100 / (total ? total : 1));
  if (pct / 10 != lastStep) {
    lastStep = pct / 10;
    sendOtaStatus(OTA_ST_PROGRESS, pct, 0);
    sendStatus();          // Master sieht uns weiter als aktiv (Flag OTA_BUSY)
    Serial.printf("[OTA] %u %%\n", pct);
  }
}

// Eigentlicher Download. Rueckgabe OTA_ERR_NONE = geflasht und geprueft.
// Bei Fehler raeumt doHttpOta() auf (Update.abort()).
uint8_t otaDownload(NetworkClient &c) {
  uint32_t t0 = millis();
  wdtFeed();
  if (!c.connect(masterIP, 80, OTA_CONNECT_TIMEOUT_MS)) return OTA_ERR_CONNECT;
  wdtFeed();
  static const char req[] = "GET /fw/node.bin HTTP/1.0\r\nHost: " MASTER_IP "\r\nConnection: close\r\n\r\n";
  if (c.write((const uint8_t *)req, sizeof(req) - 1) != sizeof(req) - 1) return OTA_ERR_CONNECT;
  wdtFeed();

  // --- Statuszeile + Header (gemeinsame Deadline 5 s) ---
  char line[160];
  uint32_t th = millis();
  int n = otaReadLine(c, line, sizeof(line), th);
  if (n < 0) { Serial.println("[OTA] Keine HTTP-Antwort"); return OTA_ERR_HTTP; }
  int code = 0;
  if (strncmp(line, "HTTP/", 5) == 0) {
    const char *sp = strchr(line, ' ');
    if (sp) code = atoi(sp + 1);
  }
  if (code != 200) { Serial.printf("[OTA] HTTP-Antwort: %s\n", line); return OTA_ERR_HTTP; }

  uint32_t len = 0;
  bool haveLen = false;
  char md5[33] = "";
  for (;;) {
    n = otaReadLine(c, line, sizeof(line), th);
    if (n < 0) { Serial.println("[OTA] Header unvollstaendig"); return OTA_ERR_HTTP; }
    if (n == 0) break;                                    // Leerzeile = Ende der Header
    if (strncasecmp(line, "Content-Length:", 15) == 0) {
      haveLen = otaParseLen(line + 15, &len);
    } else if (strncasecmp(line, "x-MD5:", 6) == 0) {
      if (!otaParseMd5(line + 6, md5)) Serial.println("[OTA] x-MD5 ungueltig – ignoriert");
    }
  }
  if (!haveLen || len == 0 || len > CTF_APP_MAX_SIZE) {
    Serial.printf("[OTA] Content-Length fehlt/ungueltig (%lu)\n", (unsigned long)len);
    return OTA_ERR_SIZE;
  }
  if (len < OTA_HDR_LEN) { Serial.println("[OTA] Datei zu klein fuer eine Firmware"); return OTA_ERR_IMAGE; }

  // --- Erste 0x140 Byte lesen und pruefen, BEVOR Update.begin() den Flash anfasst ---
  uint32_t got = 0;
  while (got < OTA_HDR_LEN) {
    int r = otaRead(c, otaBuf + got, OTA_HDR_LEN - got, t0);
    if (r <= 0) return OTA_ERR_DOWNLOAD;
    got += (uint32_t)r;
  }
  CtfFwDesc d;
  memcpy(&d, otaBuf + CTF_DESC_OFFSET, sizeof(d));
  d.version[sizeof(d.version) - 1] = 0;
  uint16_t chip = (uint16_t)(otaBuf[12] | (otaBuf[13] << 8));
  const char *bad = NULL;
  if (otaBuf[0] != 0xE9)                         bad = "kein ESP32-App-Image (bitte node.ino.bin verwenden)";
  else if (otaBuf[1] < 1 || otaBuf[1] > 16)      bad = "Image-Header defekt";
  else if (chip != OTA_CHIP_ID)                  bad = "Firmware fuer anderen Chip-Typ";
  else if (rdLE32(otaBuf + 0x20) != 0xABCD5432UL) bad = "kein App-Image (bitte node.ino.bin verwenden)";
  else if (d.magic != CTF_DESC_MAGIC)            bad = "keine CTF-Firmware (zu alt?)";
  else if (d.role != CTF_ROLE_NODE)              bad = "keine Node-Firmware (Master-Firmware?)";
  else if (d.proto != PROTO_VERSION)             bad = "falsches Protokoll";
  if (bad) {
    Serial.printf("[OTA] Datei abgelehnt: %s\n", bad);
    if (d.magic == CTF_DESC_MAGIC)
      Serial.printf("[OTA]   (Rolle %u, Protokoll %u, Version %s; erwartet Rolle %u, Protokoll %u)\n",
                    d.role, d.proto, d.version, CTF_ROLE_NODE, PROTO_VERSION);
    return OTA_ERR_IMAGE;
  }
  Serial.printf("[OTA] Node-Firmware %s, %lu Byte, MD5 %s\n",
                d.version, (unsigned long)len, md5[0] ? md5 : "-");

  // --- Flashen ---
  if (Update.isRunning()) Update.abort();   // Reste eines frueheren Versuchs
  if (!Update.begin(len)) { Serial.printf("[OTA] Update.begin: %s\n", Update.errorString()); return OTA_ERR_BEGIN; }
  if (md5[0]) Update.setMD5(md5);
  if (Update.write(otaBuf, OTA_HDR_LEN) != OTA_HDR_LEN) {
    Serial.printf("[OTA] Schreibfehler: %s\n", Update.errorString());
    return OTA_ERR_WRITE;
  }
  uint32_t done = OTA_HDR_LEN;
  uint8_t lastStep = 0;
  otaProgress(done, len, lastStep);
  while (done < len) {
    uint32_t want = len - done;
    if (want > sizeof(otaBuf)) want = sizeof(otaBuf);
    int r = otaRead(c, otaBuf, want, t0);
    if (r <= 0) break;                                    // Timeout/Abbruch -> unten
    if (Update.write(otaBuf, (size_t)r) != (size_t)r) {
      Serial.printf("[OTA] Schreibfehler: %s\n", Update.errorString());
      return OTA_ERR_WRITE;
    }
    done += (uint32_t)r;
    otaProgress(done, len, lastStep);
  }
  if (done != len) {
    Serial.printf("[OTA] Download unvollstaendig: %lu von %lu Byte\n", (unsigned long)done, (unsigned long)len);
    return OTA_ERR_DOWNLOAD;
  }
  if (!Update.end(true)) {
    Serial.printf("[OTA] Pruefung fehlgeschlagen: %s\n", Update.errorString());
    return OTA_ERR_VERIFY;
  }
  return OTA_ERR_NONE;
}

void doHttpOta() {
  Serial.printf("[OTA] Update vom Master angefordert (%lu Byte)\n", (unsigned long)otaAnnounced);
  nodeFlags  = (nodeFlags | NODE_FLAG_OTA_BUSY) & ~NODE_FLAG_OTA_FAILED;
  btnPending = false;
  sendOtaStatus(OTA_ST_STARTED, 0, 0);
  delay(5);
  sendOtaStatus(OTA_ST_STARTED, 0, 0);       // doppelt gegen Paketverlust
  barLit = -1;
  drawProgressBar(0, 1);                     // lila = Update laeuft

  NetworkClient c;
  uint8_t err = otaDownload(c);
  c.stop();

  if (err == OTA_ERR_NONE) {
    Serial.println("[OTA] Erfolgreich – Neustart");
    applyColor(COLORS[COL_GREEN]);
    for (uint8_t i = 0; i < 3; i++) { sendOtaStatus(OTA_ST_SUCCESS, 100, 0); delay(20); }
    delay(300);
    ESP.restart();
    return;
  }

  // Fehler: aufraeumen und normal weitermachen
  Update.abort();                            // sicher, auch wenn nie gestartet
  Serial.printf("[OTA] FEHLGESCHLAGEN (Code %u: %s)\n", err, otaErrText(err));
  udpDiscardAll();                           // veraltete Pakete (z.B. wiederholte PKT_OTA) verwerfen
  for (uint8_t i = 0; i < 3; i++) { sendOtaStatus(OTA_ST_FAILED, 0, err); delay(20); }
  nodeFlags = (nodeFlags & ~NODE_FLAG_OTA_BUSY) | NODE_FLAG_OTA_FAILED;
  startOverlay(OV_ERROR, OTA_ERROR_SHOW_MS); // ~2 s rot blinken
  ledForceRedraw();
  ledApplied = 0;                            // Master sendet den Spielzustand neu
  lastPong   = millis();                     // waehrend des Downloads kam kein PONG
  sendStatus();
}
#endif

// ─────────────────────────────────────────────────────────────
// WiFi connection
// ─────────────────────────────────────────────────────────────

void wifiBegin() {
  if (strlen(WIFI_PASS) > 0) WiFi.begin(WIFI_SSID, WIFI_PASS);
  else WiFi.begin(WIFI_SSID);
}

// Erste Verbindung beim Start (blockiert bis verbunden, gelb blinkend)
void connectWiFiFirst() {
  Serial.printf("\nConnecting to '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  // WLAN-Stromsparmodus AUS – verhindert verpasste UDP-Pakete (Hauptursache fuer
  // "Node reagiert nicht"). Ohne dies schlaeft der Funkchip periodisch ein.
#ifdef ESP32
  WiFi.setSleep(false);
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  WiFi.setAutoReconnect(true);
  wifiBegin();

  setLed(COL_YELLOW, ANIM_BLINK_SLOW);   // gelb blinken = sucht WLAN
  uint32_t start = millis(), lastDot = millis();
  while (WiFi.status() != WL_CONNECTED) {
    wdtFeed();
    updateLed();
    if (millis() - start >= WIFI_TIMEOUT_MS) {
      Serial.println("\n[WARN] WiFi timeout, retrying...");
      WiFi.disconnect();
      delay(200);
      wifiBegin();
      start = millis();
    }
    if (millis() - lastDot >= 500) { lastDot = millis(); Serial.print('.'); }
    delay(20);
  }
  Serial.printf("\nConnected. IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
}

// Laufende WLAN-Ueberwachung (blockiert nie). Die ID bleibt bei WLAN-Verlust
// erhalten – der Master kennt uns per MAC; kennt er uns nicht mehr, antwortet er RESET.
void wifiService() {
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) {
      wifiUp      = true;
      wifiOverlay = false;
      Serial.printf("[WLAN] Wieder verbunden nach %lu s, IP %s\n",
                    (unsigned long)((now - wifiLostSince) / 1000), WiFi.localIP().toString().c_str());
      if (myId == 0) {
        nextRegisterAt = now + (uint32_t)random(0, 501);
      } else {
        lastPong = now;
        sendStatus();
      }
    }
    return;
  }
  if (wifiUp) {
    wifiUp        = false;
    wifiLostSince = now;
    lastReconnect = now;
    btnPending    = false;
    Serial.printf("[WLAN] Verbindung verloren (ID %u bleibt erhalten)\n", myId);
  }
  if (!wifiOverlay && now - wifiLostSince > WIFI_OVERLAY_MS) wifiOverlay = true;
  if (now - lastReconnect >= WIFI_RECONNECT_MS) {
    lastReconnect = now;
    Serial.println("[WLAN] Immer noch getrennt – WiFi.reconnect()");
    WiFi.reconnect();
  }
}

// ─────────────────────────────────────────────────────────────
// Boot-Infos
// ─────────────────────────────────────────────────────────────

const char *resetReasonText(uint8_t r) {
  static const char *const T[16] = {
    "unbekannt", "Einschalten", "Reset-Pin", "Software-Neustart", "ABSTURZ",
    "Interrupt-Watchdog", "Task-Watchdog", "Watchdog", "Deep-Sleep",
    "BROWNOUT – Stromversorgung pruefen!", "SDIO", "USB", "JTAG", "eFuse",
    "Spannungseinbruch", "CPU-Lockup"
  };
  return T[r & 0x0F];
}

void readIdentity() {
#ifdef ESP32
  esp_read_mac(myMac, ESP_MAC_WIFI_STA);   // WiFi.macAddress() kann vor dem Start 0 sein
  int rr = (int)esp_reset_reason();
  resetReason = (uint8_t)((rr < 0) ? 0 : (rr > 15 ? 15 : rr));
#else
  WiFi.macAddress(myMac);
  resetReason = 0;
#endif
}

// ─────────────────────────────────────────────────────────────
// ArduinoOTA (Update per Arduino IDE ueber WLAN)
// ─────────────────────────────────────────────────────────────

void setupArduinoOta() {
  static char hostname[20];
  snprintf(hostname, sizeof(hostname), "ctf-node-%02x%02x%02x", myMac[3], myMac[4], myMac[5]);
  ArduinoOTA.setHostname(hostname);
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  Serial.printf("[OTA] Hostname: %s%s\n", hostname, strlen(OTA_PASSWORD) > 0 ? " (mit Passwort)" : "");

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Start");
    barLit = -1;
    drawProgressBar(0, 1);
  });
  ArduinoOTA.onEnd([]() {
    applyColor(COLORS[COL_GREEN]);
    Serial.println("[OTA] Fertig – Neustart");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    wdtFeed();
    drawProgressBar(progress, total);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] Fehler %u\n", (unsigned)e);
    startOverlay(OV_ERROR, OTA_ERROR_SHOW_MS);
    ledForceRedraw();
  });
  ArduinoOTA.begin();
}

// ─────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);
  readIdentity();
#ifdef ESP32
  // ctf_fw_desc.version MUSS hier benutzt werden: ohne Verweis entfernt der Linker
  // (--gc-sections) die Firmware-Kennung aus dem .bin -> OTA-Pruefung schlaegt fehl!
  Serial.printf("\n=== ESP CTF Game – Node v%s (Protokoll %u) ===\n", ctf_fw_desc.version, PROTO_VERSION);
#else
  Serial.printf("\n=== ESP CTF Game – Node v%s (Protokoll %u) ===\n", CTF_FW_VERSION, PROTO_VERSION);
#endif
  Serial.printf("MAC %02X:%02X:%02X:%02X:%02X:%02X, Reset-Grund %u (%s)\n",
                myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5],
                resetReason, resetReasonText(resetReason));
#ifdef ESP32
  {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY)
      Serial.println("[FW] Neue Firmware – wird nach der Registrierung bestaetigt (sonst Rollback)");
  }
#endif

#if defined(ESP32) && USE_WATCHDOG
  {
    esp_task_wdt_config_t c = {.timeout_ms = (uint32_t)WDT_TIMEOUT_S * 1000UL, .idle_core_mask = 1, .trigger_panic = true};
    if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
    enableLoopWDT();
    Serial.printf("[WDT] Watchdog aktiv (%u s)\n", (unsigned)WDT_TIMEOUT_S);
  }
#endif

#ifndef ESP32
  randomSeed(ESP.getChipId() ^ micros());   // ESP32: random() nutzt den Hardware-Zufall
#endif

  masterIP.fromString(MASTER_IP);

  // LED init
#ifdef LED_NEOPIXEL
  strip.begin();
  strip.setBrightness(80);
  strip.show();
#else
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
#endif

  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Startup blink
  applyColor(0xFF8000); delay(200); applyColor(0); delay(200);
  applyColor(0xFF8000); delay(200); applyColor(0);

  connectWiFiFirst();
#ifndef ESP32
  WiFi.macAddress(myMac);   // ESP8266: MAC erst nach WiFi.mode() sicher gueltig
#endif
  wifiUp = true;
  if (!udp.begin(UDP_PORT)) Serial.println("[WARN] UDP-Port konnte nicht geoeffnet werden");

  setupArduinoOta();

  // Nicht registriert: blau langsam blinken. Erste Registrierung mit Zufallspause,
  // damit nicht alle Nodes gleichzeitig senden (z.B. nach gemeinsamem Einschalten).
  setLed(COL_BLUE, ANIM_BLINK_SLOW);
  btnNonce       = (uint8_t)random(1, 256);
  nextRegisterAt = millis() + (uint32_t)random(0, 1001);
}

void loop() {
  wifiService();

  if (wifiUp) {
    ArduinoOTA.handle();
    handleUDP();
#ifdef ESP32
    if (otaRequested) {
      otaRequested = false;
      if (myId != 0) doHttpOta();
    }
#endif
    registrationService();
    statusService();
  }

  // Taster – Druck IMMER erkennen (Diagnose), gesendet wird nur registriert + mit WLAN
  if (buttonPressed()) onButtonPress();
  buttonRetryService();

  // Rollback-Schutz: nach 90 s Laufzeit gilt die Firmware auch ohne Registrierung als gut
  if (!fwValid && millis() >= ROLLBACK_OK_MS) markFwValid("90 s Laufzeit");

  renderLed();
  yield();  // ESP8266 watchdog
}
