/*
 * ESP CTF Game – Node Firmware (v3, ESP-NOW)
 * Nur ESP32.
 *
 * Each node:
 *   - Sucht den Master per WLAN-Scan (SSID aus config.h) und funkt dann per ESP-NOW auf
 *     dessen Kanal. Kanal + Master-MAC werden gespeichert -> nach einem Neustart sofort wieder da.
 *   - Registers itself by MAC address and receives a stable node ID
 *   - Monitors a button (with debounce), presses are delivered with ACK + retries
 *   - Drives an RGB LED (NeoPixel WS2812B-8 bar or 3-pin RGB)
 *   - Executes animations (solid, blink, pulse, flash, bar, split)
 *   - Sends a STATUS every 2 s (LED-Seq, signal, firmware version, reset reason)
 *   - Update per USB oder vom Master (die Node verbindet sich dafuer kurz mit dem Master-WLAN)
 *   - Update-Modus: passt das Spiel-Protokoll nicht zum Master, kann die Node nur aktualisiert werden
 *
 * LED:  gelb blinkt      = sucht den Master
 *       blau langsam     = Master gefunden, wartet auf Anmeldung
 *       gruen Blitz      = angemeldet
 *       lila langsam     = Update-Modus ("Update noetig")
 *       lila Balken      = Update laeuft
 *       rot blinkt (2 s) = Update fehlgeschlagen
 */

// config.h MUSS zuerst kommen – definiert LED_NEOPIXEL, Pins, etc.
#include "config.h"

#ifndef ESP32
  #error "Die Node-Firmware ab v3 (ESP-NOW) laeuft nur auf ESP32 (Board z.B. 'ESP32 Dev Module')."
#endif

// Fallbacks fuer aeltere/unvollstaendige config.h-Kopien
#if !defined(LED_NEOPIXEL) && !defined(LED_RGB_CATHODE) && !defined(LED_RGB_ANODE)
  #define LED_NEOPIXEL
#endif
#ifndef NEO_PIN
  #define NEO_PIN 13
#endif
#ifndef LED_R_PIN
  #define LED_R_PIN 25
#endif
#ifndef LED_G_PIN
  #define LED_G_PIN 26
#endif
#ifndef LED_B_PIN
  #define LED_B_PIN 27
#endif
#ifndef BUTTON_PIN
  #define BUTTON_PIN 18
#endif
#ifndef NEO_COUNT
  #define NEO_COUNT 8
#endif
#ifndef BUTTON_DEBOUNCE_MS
  #define BUTTON_DEBOUNCE_MS 50
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID "ESP-CTF-Game"
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS ""
#endif
#ifndef MASTER_IP
  #define MASTER_IP "192.168.4.1"
#endif
#ifndef USE_WATCHDOG
  #define USE_WATCHDOG 1         // 1 = Watchdog startet die Node neu, falls sie haengt
#endif
#ifndef WDT_TIMEOUT_S
  #define WDT_TIMEOUT_S 30
#endif

#include "protocol.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stddef.h>
// Vorab-Deklaration mit C-Linkage – MUSS vor den automatisch erzeugten
// Prototypen der Arduino-IDE stehen (sonst "conflicting declaration").
extern "C" bool verifyRollbackLater();

#ifdef LED_NEOPIXEL
  #include <Adafruit_NeoPixel.h>
#endif

// Zeiten (Teil des Protokolls – nur zusammen mit dem Master aendern)
#define STATUS_INTERVAL_MS     2000UL   // STATUS/Keepalive an den Master
#define MASTER_LOST_MS        20000UL   // so lange kein PONG -> Master suchen (Master: Timeout 15 s)
#define REGISTER_INTERVAL_MS   1000UL   // Registrierung wiederholen (+ Zufall 0..300 ms)
#define REGISTER_BURST            3     // die ersten Versuche schnell hintereinander ...
#define REGISTER_BURST_GAP_MS   300UL   // ... im Abstand von 300 ms (3 Versuche in ~900 ms)
#define CACHE_TRY_MS           1000UL   // so lange nur den gespeicherten Master probieren, dann scannen
#define RESET_SCAN_DELAY_MS    5000UL   // nach RESET/Desync (Master ist da): erst nach 5 s scannen
#define SCAN_INTERVAL_MS      10000UL   // WLAN-Scan wiederholen, solange nicht angemeldet ...
#define SCAN_JITTER_MS         2000UL   // ... plus Zufall 0..2 s
#define SCAN_RETRY_MS          2000UL   // Scan liess sich nicht starten -> so bald erneut
#define SCAN_MS_PER_CHANNEL     150     // aktiver Scan: ~2 s fuer alle Kanaele
#define SCAN_TIMEOUT_MS        8000UL   // Scan dauert laenger -> abbrechen
#define ROLLBACK_OK_MS        90000UL   // spaetestens dann gilt neue Firmware als gut
#define OTA_ERROR_SHOW_MS      2000UL   // rot blinken nach OTA-Fehler
#define DIAG_INTERVAL_MS      60000UL   // Funk-Statistik (nur bei Aenderung) im Seriellen Monitor
// HTTP-OTA
#define OTA_WIFI_TIMEOUT_MS   20000UL   // so lange auf Verbindung + IP vom Master-WLAN warten
#define OTA_WIFI_RETRY_MS      5000UL   // nicht verbunden -> Verbindungsversuch wiederholen (ESP-IDF versucht nur 1x)
#define OTA_CONNECT_TIMEOUT_MS 5000
#define OTA_HEADER_TIMEOUT_MS 15000UL   // Statuszeile + Header (Master bedient nur einen Web-Client gleichzeitig)
#define OTA_IDLE_TIMEOUT_MS   10000UL   // so lange keine Daten -> Abbruch
#define OTA_TOTAL_TIMEOUT_MS 120000UL   // Gesamtdauer Download
#define OTA_HDR_LEN           0x140     // so viele Byte werden VOR Update.begin() geprueft

#define RX_QUEUE_LEN 32                 // empfangene Funkrahmen (WLAN-Task -> loop)

static_assert(sizeof(Packet) == 8, "Packet muss 8 Byte gross sein");
static_assert(sizeof(CtfFrame) == 10 && offsetof(CtfFrame, p) == 2, "CtfFrame muss 10 Byte gross sein");
static_assert(offsetof(CtfFwDesc, otaCompat) == 9, "CtfFwDesc-Layout falsch");
static_assert(CTF_DESC_OFFSET + sizeof(CtfFwDesc) <= OTA_HDR_LEN, "OTA_HDR_LEN zu klein");

// Ein empfangener Funkrahmen (vom WLAN-Task in die Queue kopiert)
struct RxItem {
  uint8_t src[6];   // Absender-MAC
  int8_t  rssi;     // Signalstaerke dieses Rahmens (dBm)
  Packet  p;
};

// ─────────────────────────────────────────────────────────────
// Firmware-Kennung + Rollback
// ─────────────────────────────────────────────────────────────
// Landet im .bin bei Offset 0x120. Master und Node pruefen damit Firmware-Dateien
// (richtige Rolle, OTA-kompatibel) bevor sie geflasht werden.
extern "C" const CtfFwDesc ctf_fw_desc __attribute__((section(".rodata_custom_desc"), used)) =
  { CTF_DESC_MAGIC, CTF_ROLE_NODE, PROTO_VERSION, CTF_FW_MAJOR, CTF_FW_MINOR, CTF_FW_PATCH, OTA_COMPAT, {0, 0}, CTF_FW_VERSION };

// Neue Firmware gilt erst als gut, wenn sie sich beim Master registriert hat (oder
// 90 s laeuft). Stuerzt sie vorher ab, bootet beim naechsten Start die alte Firmware.
extern "C" bool verifyRollbackLater() { return true; }

#ifdef CONFIG_IDF_FIRMWARE_CHIP_ID
  #define OTA_CHIP_ID CONFIG_IDF_FIRMWARE_CHIP_ID
#else
  #define OTA_CHIP_ID 0
#endif

bool fwValid = false;   // Firmware bereits als gueltig markiert?

void markFwValid(const char *why) {
  if (fwValid) return;
  fwValid = true;
  esp_ota_mark_app_valid_cancel_rollback();   // harmlos, falls kein Rollback ansteht
  Serial.printf("[FW] Firmware %s bestaetigt (%s)\n", CTF_FW_VERSION, why);
}

// Watchdog fuettern (nur noetig in langen Schleifen: OTA)
void wdtFeed() {
#if USE_WATCHDOG
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
// wird es tausende Male/s aufgerufen -> der Funk kann keine Pakete
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
      // 30fps – haeufiger ist fuer das Auge nicht sichtbar aber kostet Funk-Zeit
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
#define OV_REDRAW   0xFF

uint8_t  ovMode      = OV_NONE;   // Timer-Overlay: OV_IDENTIFY oder OV_ERROR
uint32_t ovUntil     = 0;
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
  uint8_t ov = ovMode;
  if (ov != ovShown) {
    ovShown  = ov;
    ovSince  = now;
    ovLastOn = -1;
    if (ov == OV_NONE) led.dirty = true;   // normale Anzeige wiederherstellen
  }
  if (ov == OV_NONE) { updateLed(); return; }

  uint32_t col = (ov == OV_ERROR) ? COLORS[COL_RED] : COLORS[COL_WHITE];
  int8_t on = (((now - ovSince) / 125) % 2 == 0) ? 1 : 0;
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
// Zustand
// ─────────────────────────────────────────────────────────────

#define MODE_SEARCH      0   // kein Master: suchen + registrieren
#define MODE_REGISTERED  1   // angemeldet, Spielbetrieb
#define MODE_UPDATE_ONLY 2   // angemeldet, aber Protokoll passt nicht: nur Firmware-Update

uint8_t   nodeMode       = MODE_SEARCH;
uint8_t   myMac[6]       = {0};
uint8_t   resetReason    = 0;      // esp_reset_reason(), 0..15
uint8_t   myId           = 0;      // 0 = nicht registriert
uint8_t   ledApplied     = 0;      // zuletzt angewendete LED-Seq (0 = keine)
uint8_t   nodeFlags      = 0;      // NODE_FLAG_OTA_BUSY / NODE_FLAG_OTA_FAILED
uint32_t  lastStatus     = 0;
uint32_t  lastPong       = 0;
int8_t    lastRssi       = 0;      // Signal des letzten Rahmens vom Master (dBm)

// Master (AP-MAC = BSSID seines WLANs + Funkkanal), aus dem Speicher oder per Scan
bool      haveMaster     = false;
uint8_t   masterMac[6]   = {0};
uint8_t   masterCh       = 0;
bool      masterSeen     = false;  // SEARCH: Master per Scan/Funk bestaetigt (LED blau statt gelb)

// Suche / Registrierung
uint32_t  nextRegisterAt = 0;      // naechste Registrierung (millis)
uint8_t   regBurst       = 0;      // noch so viele schnelle Wiederholungen
uint16_t  regAttempts    = 0;
uint32_t  nextScanAt     = 0;      // naechster WLAN-Scan (millis)
bool      scanRunning    = false;
uint32_t  scanStartedAt  = 0;
uint16_t  scanMisses     = 0;      // Scans ohne Treffer in Folge (fuer sparsame Logs)
uint8_t   scanFails      = 0;      // Scan-Fehler in Folge (-> Funk-Neustart)

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
uint8_t   otaChannel   = 0;        // WLAN-Kanal laut PKT_OTA (0 = aktueller)

// Funk: Empfangs-Queue + Zaehler (Diagnose)
QueueHandle_t     rxQueue   = NULL;
volatile uint32_t rxDrops   = 0;   // Queue voll (nur im WLAN-Task geschrieben)
volatile uint32_t txFail    = 0;   // Master hat nicht quittiert (nur im WLAN-Task geschrieben)
uint32_t          txErr     = 0;   // esp_now_send() abgelehnt (z.B. Sendepuffer voll)
uint32_t          rxForeign = 0;   // Rahmen von fremden Absendern
uint32_t          lastDiagAt = 0;
uint32_t          diagShown[4] = {0, 0, 0, 0};

Preferences prefs;

// ─────────────────────────────────────────────────────────────
// Funk (ESP-NOW)
// ─────────────────────────────────────────────────────────────

// Laeuft im WLAN-Task: NUR pruefen und in die Queue kopieren
// (kein Serial, keine Spiellogik, kein Senden).
void onNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (rxQueue == NULL || info == NULL || info->src_addr == NULL || data == NULL) return;
  if (len != (int)sizeof(CtfFrame) || data[0] != CTF_FRAME_MAGIC0 || data[1] != CTF_FRAME_MAGIC1) return;
  RxItem it;
  memcpy(it.src, info->src_addr, 6);
  it.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;
  memcpy(&it.p, data + offsetof(CtfFrame, p), sizeof(Packet));
  if (xQueueSend(rxQueue, &it, 0) != pdTRUE) rxDrops = rxDrops + 1;
}

// Laeuft im WLAN-Task: nur zaehlen.
void onNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  (void)info;
  if (status != ESP_NOW_SEND_SUCCESS) txFail = txFail + 1;
}

// WLAN als Station starten (NICHT verbunden) + ESP-NOW. false = Fehler.
bool radioStart() {
  WiFi.persistent(false);        // WLAN-Daten nicht im Flash speichern
  WiFi.setAutoReconnect(false);  // nur fuer Updates verbunden – nie selbst neu verbinden
  WiFi.setSleep(false);          // Funk-Stromsparmodus AUS (sonst verpasste Pakete)
  if (!WiFi.mode(WIFI_STA)) { Serial.println("[FUNK] WLAN-Start fehlgeschlagen"); return false; }
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_err_t e = esp_now_init();
  if (e != ESP_OK) { Serial.printf("[FUNK] ESP-NOW-Start fehlgeschlagen (0x%x)\n", (unsigned)e); return false; }
  esp_now_register_recv_cb(onNowRecv);
  esp_now_register_send_cb(onNowSent);
  return true;
}

void radioStop() {
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

bool masterAttach();
void rxFlush();

// Funk komplett neu starten (wie beim Einschalten) und wieder auf den Master-Kanal.
// Klappt das nicht, startet die Node neu (danach sucht sie den Master wieder).
void radioRestart(const char *why) {
  Serial.printf("[FUNK] Funk-Neustart: %s\n", why);
  radioStop();
  wdtFeed();
  if (!radioStart()) {
    Serial.println("[FUNK] Funk laesst sich nicht neu starten – Neustart der Node");
    delay(200);
    ESP.restart();
  }
  masterAttach();
  rxFlush();
}

bool validChannel(uint8_t ch) { return ch >= 1 && ch <= 14; }

// Gueltige Unicast-MAC (nicht 0, kein Broadcast/Multicast)
bool validMac(const uint8_t *m) {
  if (m[0] & 0x01) return false;
  for (uint8_t i = 0; i < 6; i++) if (m[i]) return true;
  return false;
}

// Funkkanal setzen (nur erlaubt, solange die Node NICHT mit einem WLAN verbunden ist)
bool setChannel(uint8_t ch) {
  if (!validChannel(ch)) return false;
  esp_err_t e = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  if (e != ESP_OK) { Serial.printf("[FUNK] Kanal %u setzen fehlgeschlagen (0x%x)\n", ch, (unsigned)e); return false; }
  return true;
}

// Master als ESP-NOW-Partner eintragen (Kanal 0 = aktueller Kanal)
bool addMasterPeer(bool logErr) {
  if (!haveMaster) return false;
  if (esp_now_is_peer_exist(masterMac)) return true;
  esp_now_peer_info_t pi;
  memset(&pi, 0, sizeof(pi));
  memcpy(pi.peer_addr, masterMac, 6);
  pi.channel = 0;
  pi.ifidx   = WIFI_IF_STA;
  pi.encrypt = false;
  esp_err_t e = esp_now_add_peer(&pi);
  if (e != ESP_OK && logErr) Serial.printf("[FUNK] Master als Partner eintragen fehlgeschlagen (0x%x)\n", (unsigned)e);
  return e == ESP_OK;
}

// Kanal des bekannten Masters einstellen + Partner eintragen
bool masterAttach() {
  if (!haveMaster) return false;
  bool ok = setChannel(masterCh);
  return addMasterPeer(true) && ok;
}

// Neuen Master (aus Speicher oder Scan) uebernehmen
void useMaster(const uint8_t *bssid, uint8_t ch) {
  if (haveMaster && memcmp(bssid, masterMac, 6) != 0 && esp_now_is_peer_exist(masterMac))
    esp_now_del_peer(masterMac);           // alten Master austragen
  memcpy(masterMac, bssid, 6);
  masterCh   = ch;
  haveMaster = true;
  masterAttach();
}

// Gespeicherten Master (Kanal + BSSID) laden / speichern (Preferences "ctfnode")
void cacheLoad() {
  if (!prefs.begin("ctfnode", false)) return;
  uint8_t ch = prefs.getUChar("ch", 0);
  uint8_t b[6] = {0};
  size_t n = prefs.getBytes("bssid", b, sizeof(b));
  prefs.end();
  if (n != 6 || !validChannel(ch) || !validMac(b)) return;
  useMaster(b, ch);
  Serial.printf("[SUCHE] Gespeicherter Master: %02X:%02X:%02X:%02X:%02X:%02X, Kanal %u\n",
                b[0], b[1], b[2], b[3], b[4], b[5], ch);
}

void cacheSave() {
  if (!prefs.begin("ctfnode", false)) { Serial.println("[SUCHE] Speichern fehlgeschlagen"); return; }
  prefs.putUChar("ch", masterCh);
  prefs.putBytes("bssid", masterMac, 6);
  prefs.end();
}

// Alle wartenden Funkrahmen verwerfen (nach langem Blockieren sind sie veraltet)
void rxFlush() {
  if (rxQueue != NULL) xQueueReset(rxQueue);
}

bool sendPkt(uint8_t type, uint8_t nid,
             uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
             uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  if (!haveMaster) return false;                                  // noch kein Master bekannt
  if (!esp_now_is_peer_exist(masterMac) && !addMasterPeer(false)) { txErr++; return false; }
  CtfFrame f;
  f.magic[0] = CTF_FRAME_MAGIC0;
  f.magic[1] = CTF_FRAME_MAGIC1;
  f.p.type = type; f.p.nodeId = nid;
  f.p.data[0]=d0; f.p.data[1]=d1; f.p.data[2]=d2;
  f.p.data[3]=d3; f.p.data[4]=d4; f.p.data[5]=d5;
  // Fehler (z.B. Sendepuffer voll) = "nicht gesendet": die Wiederholungslogik
  // (Status alle 2 s, LED-Wiederholung des Masters, Tasten-Wiederholung) faengt das auf.
  if (esp_now_send(masterMac, (const uint8_t *)&f, sizeof(f)) != ESP_OK) { txErr++; return false; }
  return true;
}

void sendStatus() {
  lastStatus = millis();
  if (myId == 0) return;
  uint8_t fl = nodeFlags & NODE_FLAG_MASK;
  if (identifyActive()) fl |= NODE_FLAG_IDENTIFY;
  if (nodeMode == MODE_UPDATE_ONLY) fl |= NODE_FLAG_UPDATE_ONLY;
  sendPkt(PKT_STATUS, myId, ledApplied, (uint8_t)lastRssi,
          CTF_FW_MAJOR, CTF_FW_MINOR, CTF_FW_PATCH, (uint8_t)((fl & NODE_FLAG_MASK) | (resetReason << 4)));
}

void sendOtaStatus(uint8_t st, uint8_t pct, uint8_t err) {
  sendPkt(PKT_OTA_STATUS, myId, st, pct, err);
}

// ─────────────────────────────────────────────────────────────
// Suche / Registrierung
// ─────────────────────────────────────────────────────────────

// LED waehrend der Suche: gelb = sucht, blau = Master gefunden, wartet auf Anmeldung
void searchLed() { setLed(masterSeen ? COL_BLUE : COL_YELLOW, ANIM_BLINK_SLOW); }

// ID verwerfen und (neu) suchen/registrieren. Erste Registrierung nach Zufallspause
// 0..regJitterMs. masterAlive = Master hat sich gerade gemeldet (RESET/Desync):
// dann erst spaeter scannen; sonst nach kurzem Versuch mit dem bekannten Master.
void enterSearch(const char *why, uint32_t regJitterMs, bool masterAlive) {
  uint32_t now = millis();
  if (nodeMode != MODE_SEARCH)
    Serial.printf("[NODE] %s – ID %u verworfen, melde neu an\n", why, myId);
  nodeMode       = MODE_SEARCH;
  myId           = 0;
  btnPending     = false;
  otaRequested   = false;
  regAttempts    = 0;
  regBurst       = REGISTER_BURST - 1;
  masterSeen     = masterAlive && haveMaster;
  nextRegisterAt = now + (uint32_t)random(0, (long)regJitterMs + 1);
  nextScanAt     = haveMaster ? nextRegisterAt + (masterAlive ? RESET_SCAN_DELAY_MS : CACHE_TRY_MS) : now;
  searchLed();
}

// Scan-Fehler (Start fehlgeschlagen / haengt): nach 3 in Folge den Funk neu starten
void scanFailed(const char *what) {
  if (scanFails < 255) scanFails++;
  Serial.printf("[SUCHE] %s (%u. Mal in Folge)\n", what, scanFails);
  if (scanFails >= 3) {
    scanFails = 0;
    radioRestart("WLAN-Scan klappt nicht");
  }
}

// Laufenden Scan sofort beenden und zurueck auf den Master-Kanal
void scanAbort() {
  if (!scanRunning) return;
  scanRunning = false;
  esp_wifi_scan_stop();
  WiFi.scanDelete();
  if (haveMaster) setChannel(masterCh);
}

void startScan() {
  uint32_t now = millis();
  int16_t r = WiFi.scanNetworks(true, false, false, SCAN_MS_PER_CHANNEL, 0, WIFI_SSID);
  if (r == WIFI_SCAN_RUNNING) {
    scanRunning   = true;
    scanStartedAt = now;
    return;
  }
  WiFi.scanDelete();
  if (haveMaster) setChannel(masterCh);
  nextScanAt = now + SCAN_RETRY_MS + (uint32_t)random(0, 501);
  scanFailed("WLAN-Scan konnte nicht starten");
}

// Laufenden Scan auswerten (danach immer zurueck auf den Master-Kanal)
void scanService() {
  if (!scanRunning) return;
  uint32_t now = millis();
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING && now - scanStartedAt < SCAN_TIMEOUT_MS) return;   // laeuft noch
  if (n < 0) {                               // haengt oder fehlgeschlagen
    scanAbort();
    nextScanAt = now + SCAN_RETRY_MS + (uint32_t)random(0, 501);
    scanFailed(n == WIFI_SCAN_RUNNING ? "WLAN-Scan dauert zu lange – abgebrochen" : "WLAN-Scan fehlgeschlagen");
    return;
  }
  scanRunning = false;
  scanFails   = 0;

  // Staerkstes WLAN mit unserer SSID gewinnt
  int16_t best = -1;
  int32_t bestRssi = -1000;
  uint8_t bssid[6] = {0};
  uint8_t ch = 0;
  for (int16_t i = 0; i < n && i < 255; i++) {
    if (WiFi.SSID((uint8_t)i) != WIFI_SSID) continue;
    int32_t c = WiFi.channel((uint8_t)i);
    if (c < 1 || c > 14) continue;
    int32_t r = WiFi.RSSI((uint8_t)i);
    if (r > bestRssi) { bestRssi = r; best = i; }
  }
  if (best >= 0) {
    WiFi.BSSID((uint8_t)best, bssid);
    ch = (uint8_t)WiFi.channel((uint8_t)best);
  }
  WiFi.scanDelete();

  if (nodeMode != MODE_SEARCH) {             // inzwischen angemeldet: nur Kanal zuruecksetzen
    if (haveMaster) setChannel(masterCh);
    return;
  }
  nextScanAt = now + SCAN_INTERVAL_MS + (uint32_t)random(0, (long)SCAN_JITTER_MS + 1);

  if (best >= 0 && validMac(bssid)) {
    bool changed = !haveMaster || ch != masterCh || memcmp(bssid, masterMac, 6) != 0;
    useMaster(bssid, ch);
    if (changed) {
      cacheSave();
      Serial.printf("[SUCHE] Master gefunden: %02X:%02X:%02X:%02X:%02X:%02X, Kanal %u, %d dBm\n",
                    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], ch, (int)bestRssi);
    }
    if (!masterSeen || changed) {            // neu gefunden: sofort schnell registrieren
      regBurst       = REGISTER_BURST - 1;
      nextRegisterAt = now;
      regAttempts    = 0;
    }
    masterSeen = true;
    scanMisses = 0;
  } else {
    if (haveMaster) setChannel(masterCh);    // weiter beim bekannten Master versuchen
    masterSeen = false;
    if (scanMisses == 0 || (scanMisses % 6) == 0)
      Serial.printf("[SUCHE] WLAN '%s' nicht gefunden – Master an? (Versuch %u)\n", WIFI_SSID, scanMisses + 1);
    if (scanMisses < 60000) scanMisses++;
  }
  searchLed();
}

// SEARCH: REGISTER an den (vermuteten) Master senden, regelmaessig scannen
void searchService() {
  if (nodeMode != MODE_SEARCH || scanRunning) return;   // waehrend des Scans nicht senden
  uint32_t now = millis();
  if ((int32_t)(now - nextScanAt) >= 0) { startScan(); return; }
  if (!haveMaster || (int32_t)(now - nextRegisterAt) < 0) return;
  if (regBurst > 0) {
    regBurst--;
    nextRegisterAt = now + REGISTER_BURST_GAP_MS;
  } else {
    nextRegisterAt = now + REGISTER_INTERVAL_MS + (uint32_t)random(0, 301);
  }
  if (regAttempts < 60000) regAttempts++;
  sendPkt(PKT_REGISTER, PROTO_VERSION, myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  if (regAttempts == 1 || regAttempts % 10 == 0)
    Serial.printf("[SUCHE] Registriere bei %02X:%02X:%02X:%02X:%02X:%02X (Kanal %u) ... (Versuch %u)\n",
                  masterMac[0], masterMac[1], masterMac[2], masterMac[3], masterMac[4], masterMac[5],
                  masterCh, regAttempts);
}

// Angemeldet (Spiel- oder Update-Modus): STATUS alle 2 s, Master-Verlust erkennen
void statusService() {
  if (nodeMode == MODE_SEARCH) return;
  uint32_t now = millis();
  if (now - lastStatus >= STATUS_INTERVAL_MS) sendStatus();
  if (now - lastPong >= MASTER_LOST_MS) enterSearch("Master antwortet nicht (20 s kein PONG)", 500, false);
}

// ─────────────────────────────────────────────────────────────
// Empfang
// ─────────────────────────────────────────────────────────────

// Unicast-Befehl pruefen: fuer uns? Fremde ID (nicht 0xFF) = Desync -> neu registrieren.
bool forMe(const Packet &p) {
  if (myId == 0) return false;               // nicht registriert: ignorieren
  if (p.nodeId == myId) return true;
  if (p.nodeId != 0xFF) {
    Serial.printf("[NODE] Paket 0x%02X fuer ID %u, eigene ID %u\n", p.type, p.nodeId, myId);
    enterSearch("Desync", 300, true);
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
      // Nur annehmen, wenn die MAC unsere ist. Anderes Spiel-Protokoll -> Update-Modus.
      if (memcmp(&p.data[2], &myMac[2], 4) != 0) return;
      uint8_t id = p.data[0];
      if (id == 0 || id == 0xFF) return;
      uint8_t prevMode = nodeMode;
      scanAbort();                              // falls gerade gescannt wird: sofort zurueck auf den Kanal
      nodeMode    = (p.data[1] == PROTO_VERSION) ? MODE_REGISTERED : MODE_UPDATE_ONLY;
      myId        = id;
      ledApplied  = 0;                          // Master sendet den Soll-Zustand neu
      { uint8_t nn; do nn = (uint8_t)random(1, 256); while (nn == btnNonce); btnNonce = nn; }   // neue Tasten-Sitzung (immer anderer Wert)
      pressSeq    = 0;
      btnPending  = false;
      lastPong    = now;
      regAttempts = 0;
      masterSeen  = true;
      if (nodeMode != prevMode) {
        if (nodeMode == MODE_REGISTERED) {
          Serial.printf("[NODE] Registriert als Node %u (Signal %d dBm)\n", myId, (int)lastRssi);
          setLed(COL_GREEN, ANIM_FLASH);
        } else {
          Serial.printf("[NODE] Registriert als Node %u – UPDATE-MODUS: Master hat Protokoll %u, "
                        "diese Node %u -> Firmware aktualisieren (Web-UI des Masters)\n",
                        myId, p.data[1], PROTO_VERSION);
          setLed(COL_PURPLE, ANIM_BLINK_SLOW);  // lila langsam = Update noetig
        }
      }
      markFwValid("Registrierung");
      sendStatus();
      return;
    }

    case PKT_RESET: {
      // nodeId 0xFF, data = Ziel-MAC (alles 0 = alle Nodes)
      if (p.nodeId != 0xFF) return;
      bool all = true;
      for (uint8_t i = 0; i < 6; i++) if (p.data[i] != 0) all = false;
      if (!all && memcmp(p.data, myMac, 6) != 0) return;
      enterSearch("Reset vom Master", 1500, true);
      return;
    }

    case PKT_SET_LED:
    case PKT_SET_BAR:
    case PKT_SET_SPLIT:
      if (nodeMode != MODE_REGISTERED || !forMe(p)) return;   // Update-Modus: keine Spielbefehle
      if (p.data[5] != ledApplied) {            // neue Seq -> anwenden
        applyLedCmd(p);
        ledApplied = p.data[5];
      }
      sendStatus();                             // Quittung (auch bei Duplikat)
      return;

    case PKT_BTN_ACK:
      if (nodeMode != MODE_REGISTERED || !forMe(p)) return;
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
      if (!otaRequested) {
        otaRequested = true;
        otaAnnounced = (uint32_t)p.data[0] | ((uint32_t)p.data[1] << 8) |
                       ((uint32_t)p.data[2] << 16) | ((uint32_t)p.data[3] << 24);
        otaChannel   = p.data[4];
      }
      return;

    default:
      return;
  }
}

// Empfangene Rahmen abarbeiten (nur vom Master; begrenzt, damit loop() nicht verhungert)
void rxService() {
  if (rxQueue == NULL) return;
  RxItem it;
  for (uint8_t guard = 0; guard < RX_QUEUE_LEN; guard++) {
    if (xQueueReceive(rxQueue, &it, 0) != pdTRUE) break;
    if (!haveMaster || memcmp(it.src, masterMac, 6) != 0) { rxForeign++; continue; }
    lastRssi = it.rssi;
    handlePacket(it.p);
  }
}

// Funk-Statistik (hoechstens jede Minute und nur, wenn sich etwas geaendert hat)
void diagService() {
  uint32_t now = millis();
  if (now - lastDiagAt < DIAG_INTERVAL_MS) return;
  lastDiagAt = now;
  uint32_t v[4] = {rxDrops, txFail, txErr, rxForeign};
  if (memcmp(v, diagShown, sizeof(v)) == 0) return;
  memcpy(diagShown, v, sizeof(v));
  Serial.printf("[FUNK] Statistik: Empfang verworfen %lu, ohne Quittung %lu, Sendefehler %lu, fremde Rahmen %lu\n",
                (unsigned long)v[0], (unsigned long)v[1], (unsigned long)v[2], (unsigned long)v[3]);
}

// ─────────────────────────────────────────────────────────────
// Taster: einmal senden, dann ohne Blockieren wiederholen bis zur Quittung
// ─────────────────────────────────────────────────────────────

void onButtonPress() {
  if (nodeMode != MODE_REGISTERED || myId == 0) {
    Serial.printf("[NODE] Taste erkannt, aber %s – nicht gesendet\n",
                  nodeMode == MODE_UPDATE_ONLY ? "Update-Modus" : "nicht registriert");
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
  if (nodeMode != MODE_REGISTERED || myId == 0 || btnTries < 1 || btnTries > BTN_MAX_SENDS) { btnPending = false; return; }
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
// HTTP-OTA vom Master: Node verbindet sich kurz mit dem Master-WLAN und laedt
// http://<MASTER_IP>/fw/node.bin. Blockiert waehrenddessen (Watchdog wird gefuettert).
// ─────────────────────────────────────────────────────────────
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
    case OTA_ERR_WIFI:     return "Master-WLAN nicht erreichbar (voll? Passwort falsch?)";
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
    rxFlush();             // waehrend des Downloads empfangene Rahmen sind veraltet
    sendOtaStatus(OTA_ST_PROGRESS, pct, 0);
    sendStatus();          // Master sieht uns weiter als aktiv (Flag OTA_BUSY)
    Serial.printf("[OTA] %u %%\n", pct);
  }
}

// Mit dem Master-WLAN verbinden (Kanal + BSSID bekannt) und auf eine IP warten.
// Meldet sich dabei alle 2 s per ESP-NOW (OTA_ST_WIFI + STATUS). true = verbunden.
bool otaJoinWifi(uint8_t ch) {
  const char *pass = (WIFI_PASS[0] != 0) ? WIFI_PASS : NULL;
  Serial.printf("[OTA] Verbinde mit WLAN '%s' (Kanal %u) ...\n", WIFI_SSID, ch);
  if (WiFi.begin(WIFI_SSID, pass, ch, masterMac) == WL_CONNECT_FAILED) {
    Serial.println("[OTA] WiFi.begin fehlgeschlagen");
    return false;
  }
  uint32_t t0 = millis(), lastSend = t0, lastTry = t0;
  wl_status_t st;
  while ((st = WiFi.status()) != WL_CONNECTED) {
    wdtFeed();
    uint32_t now = millis();
    if (now - t0 >= OTA_WIFI_TIMEOUT_MS) {
      Serial.printf("[OTA] Keine WLAN-Verbindung zum Master nach %lu s (Status %d)\n",
                    (unsigned long)(OTA_WIFI_TIMEOUT_MS / 1000), (int)st);
      return false;
    }
    // Noch nicht beim Master angemeldet (WL_IDLE_STATUS = angemeldet, wartet auf IP):
    // gescheiterten Versuch wiederholen (laeuft noch einer, lehnt der Treiber ab)
    if (now - lastTry >= OTA_WIFI_RETRY_MS && st != WL_IDLE_STATUS) {
      lastTry = now;
      Serial.printf("[OTA] WLAN-Status %d – neuer Verbindungsversuch\n", (int)st);
      WiFi.reconnect();
    }
    if (now - lastSend >= 2000) {
      lastSend = now;
      rxFlush();
      sendOtaStatus(OTA_ST_WIFI, 0, 0);
      sendStatus();
    }
    delay(20);
  }
  Serial.printf("[OTA] WLAN verbunden, IP %s, %d dBm\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

// Eigentlicher Download. Rueckgabe OTA_ERR_NONE = geflasht und geprueft.
// Bei Fehler raeumt doHttpOta() auf (Update.abort()).
uint8_t otaDownload(NetworkClient &c) {
  uint32_t t0 = millis();
  IPAddress masterIP;
  if (!masterIP.fromString(MASTER_IP)) { Serial.println("[OTA] MASTER_IP ungueltig (config.h)"); return OTA_ERR_CONNECT; }
  wdtFeed();
  if (!c.connect(masterIP, 80, OTA_CONNECT_TIMEOUT_MS)) return OTA_ERR_CONNECT;
  wdtFeed();
  static const char req[] = "GET /fw/node.bin HTTP/1.0\r\nHost: " MASTER_IP "\r\nConnection: close\r\n\r\n";
  if (c.write((const uint8_t *)req, sizeof(req) - 1) != sizeof(req) - 1) return OTA_ERR_CONNECT;
  wdtFeed();

  // --- Statuszeile + Header (gemeinsame Deadline) ---
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
  else if (d.otaCompat != OTA_COMPAT)            bad = "Firmware nicht funk-update-faehig (vor v3?)";
  if (bad) {
    Serial.printf("[OTA] Datei abgelehnt: %s\n", bad);
    if (d.magic == CTF_DESC_MAGIC)
      Serial.printf("[OTA]   (Rolle %u, OTA-Kennung %u, Version %s; erwartet Rolle %u, OTA-Kennung %u)\n",
                    d.role, d.otaCompat, d.version, CTF_ROLE_NODE, OTA_COMPAT);
    return OTA_ERR_IMAGE;
  }
  Serial.printf("[OTA] Node-Firmware %s (Protokoll %u), %lu Byte, MD5 %s\n",
                d.version, d.proto, (unsigned long)len, md5[0] ? md5 : "-");
  if (d.proto != PROTO_VERSION)
    Serial.printf("[OTA] Hinweis: anderes Spiel-Protokoll (%u statt %u) – passt es nicht zum Master, "
                  "laeuft die Node danach im Update-Modus\n", d.proto, PROTO_VERSION);

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

// Nach einem fehlgeschlagenen Update: WLAN-Verbindung beenden und den Funk komplett
// neu starten (wie beim Einschalten) -> sicher wieder ESP-NOW auf dem Master-Kanal,
// kein Verbindungsversuch laeuft mehr im Hintergrund.
void radioRecover() {
  WiFi.disconnect(false, true);              // trennen + Zugangsdaten vergessen (Funk bleibt an)
  for (uint8_t i = 0; i < 10; i++) { wdtFeed(); delay(10); }   // WLAN-Ereignisse abarbeiten lassen
  radioRestart("nach Update-Versuch");
  Serial.printf("[FUNK] Zurueck im ESP-NOW-Betrieb (Kanal %u)\n", masterCh);
}

void doHttpOta() {
  scanAbort();                               // (nur zur Sicherheit – angemeldet wird nicht gescannt)
  uint8_t ch = validChannel(otaChannel) ? otaChannel : masterCh;
  Serial.printf("[OTA] Update vom Master angefordert (%lu Byte, WLAN-Kanal %u)\n", (unsigned long)otaAnnounced, ch);
  nodeFlags  = (nodeFlags | NODE_FLAG_OTA_BUSY) & ~NODE_FLAG_OTA_FAILED;
  btnPending = false;
  sendOtaStatus(OTA_ST_WIFI, 0, 0);
  delay(5);
  sendOtaStatus(OTA_ST_WIFI, 0, 0);          // doppelt gegen Paketverlust
  barLit = -1;
  drawProgressBar(0, 1);                     // lila = Update laeuft

  uint8_t err = OTA_ERR_WIFI;
  if (otaJoinWifi(ch)) {
    sendOtaStatus(OTA_ST_STARTED, 0, 0);
    delay(5);
    sendOtaStatus(OTA_ST_STARTED, 0, 0);
    NetworkClient c;
    err = otaDownload(c);
    c.stop();
  }

  if (err == OTA_ERR_NONE) {
    Serial.println("[OTA] Erfolgreich – Neustart");
    applyColor(COLORS[COL_GREEN]);
    for (uint8_t i = 0; i < 3; i++) { sendOtaStatus(OTA_ST_SUCCESS, 100, 0); delay(20); }
    WiFi.disconnect();                       // WLAN-Platz beim Master sofort freigeben
    delay(300);
    ESP.restart();
    return;
  }

  // Fehler: aufraeumen, zurueck in den vorigen Modus (Spiel- oder Update-Modus) und weitermachen
  Update.abort();                            // sicher, auch wenn nie gestartet
  Serial.printf("[OTA] FEHLGESCHLAGEN (Code %u: %s)\n", err, otaErrText(err));
  radioRecover();
  for (uint8_t i = 0; i < 3; i++) { sendOtaStatus(OTA_ST_FAILED, 0, err); delay(20); }
  nodeFlags = (nodeFlags & ~NODE_FLAG_OTA_BUSY) | NODE_FLAG_OTA_FAILED;
  startOverlay(OV_ERROR, OTA_ERROR_SHOW_MS); // ~2 s rot blinken
  ledForceRedraw();
  ledApplied = 0;                            // Master sendet den Spielzustand neu
  lastPong   = millis();                     // waehrend des Downloads kam kein PONG
  sendStatus();
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
  esp_read_mac(myMac, ESP_MAC_WIFI_STA);   // WiFi.macAddress() kann vor dem Start 0 sein
  int rr = (int)esp_reset_reason();
  resetReason = (uint8_t)((rr < 0) ? 0 : (rr > 15 ? 15 : rr));
}

// ─────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);
  readIdentity();
  // ctf_fw_desc.version MUSS hier benutzt werden: ohne Verweis entfernt der Linker
  // (--gc-sections) die Firmware-Kennung aus dem .bin -> OTA-Pruefung schlaegt fehl!
  Serial.printf("\n=== ESP CTF Game – Node v%s (Protokoll %u, ESP-NOW) ===\n", ctf_fw_desc.version, PROTO_VERSION);
  Serial.printf("MAC %02X:%02X:%02X:%02X:%02X:%02X, Reset-Grund %u (%s)\n",
                myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5],
                resetReason, resetReasonText(resetReason));
  {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY)
      Serial.println("[FW] Neue Firmware – wird nach der Registrierung bestaetigt (sonst Rollback)");
  }

#if USE_WATCHDOG
  {
    esp_task_wdt_config_t c = {.timeout_ms = (uint32_t)WDT_TIMEOUT_S * 1000UL, .idle_core_mask = 1, .trigger_panic = true};
    if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
    enableLoopWDT();
    Serial.printf("[WDT] Watchdog aktiv (%u s)\n", (unsigned)WDT_TIMEOUT_S);
  }
#endif

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

  // Funk: WLAN-Station (nicht verbunden) + ESP-NOW
  rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(RxItem));
  if (rxQueue == NULL || !radioStart()) {
    Serial.println("[FUNK] Start fehlgeschlagen – Neustart in 3 s");
    applyColor(COLORS[COL_RED]);
    delay(3000);
    ESP.restart();
  }
  Serial.printf("[FUNK] ESP-NOW bereit, suche Master-WLAN '%s'\n", WIFI_SSID);
  cacheLoad();   // zuletzt benutzter Master (Kanal + MAC) -> meist sofort wieder angemeldet

  // Erste Registrierung mit Zufallspause, damit nicht alle Nodes gleichzeitig
  // senden (z.B. nach gemeinsamem Einschalten). Ohne gespeicherten Master: sofort scannen.
  btnNonce = (uint8_t)random(1, 256);
  enterSearch("Start", 1000, false);
}

void loop() {
  rxService();
  if (otaRequested) {
    otaRequested = false;
    if (nodeMode != MODE_SEARCH && myId != 0) doHttpOta();
  }
  searchService();
  scanService();
  statusService();

  // Taster – Druck IMMER erkennen (Diagnose), gesendet wird nur im Spielbetrieb
  if (buttonPressed()) onButtonPress();
  buttonRetryService();

  // Rollback-Schutz: nach 90 s Laufzeit gilt die Firmware auch ohne Registrierung als gut
  if (!fwValid && millis() >= ROLLBACK_OK_MS) markFwValid("90 s Laufzeit");

  diagService();
  renderLed();
}
