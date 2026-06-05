/*
 * ESP CTF Game – Node Firmware
 * Compatible with ESP8266 and ESP32.
 *
 * Each node:
 *   - Connects to the master's WiFi AP
 *   - Registers itself and receives a node ID
 *   - Monitors a button (with debounce)
 *   - Drives an RGB LED (NeoPixel WS2812B-8 bar or 3-pin RGB)
 *   - Executes animations (solid, blink, pulse, flash, bar, split)
 */

// config.h MUSS zuerst kommen – definiert LED_NEOPIXEL, Pins, etc.
#include "config.h"
#include "protocol.h"

#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#ifdef LED_NEOPIXEL
  #include <Adafruit_NeoPixel.h>
#endif

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
#define NUM_COLORS (sizeof(COLORS) / sizeof(COLORS[0]))

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
// Animation state
// ─────────────────────────────────────────────────────────────

struct LedState {
  uint8_t  colorIdx;    // COL_* index
  uint8_t  anim;        // ANIM_*
  uint32_t lastChange;
  uint8_t  phase;       // toggle/step counter
  bool     flashDone;
  uint8_t  barCount;    // for ANIM_BAR / ANIM_SPLIT: number of leading LEDs
  uint8_t  barBg;       // for ANIM_BAR: background color index (trailing LEDs)
};
LedState led = {COL_OFF, ANIM_SOLID, 0, 0, false, 0, COL_OFF};

void setLed(uint8_t colorIdx, uint8_t anim) {
  led.colorIdx   = (colorIdx < NUM_COLORS) ? colorIdx : 0;
  led.anim       = anim;
  led.lastChange = millis();
  led.phase      = 0;
  led.flashDone  = false;
}

void updateLed() {
  uint32_t color = COLORS[led.colorIdx];
  uint32_t now   = millis();

  switch (led.anim) {
    case ANIM_SOLID:
      applyColor(color);
      break;

    case ANIM_BLINK_SLOW:
      if (now - led.lastChange >= 500) {
        led.phase ^= 1;
        led.lastChange = now;
      }
      applyColor(led.phase ? color : 0);
      break;

    case ANIM_BLINK_FAST:
      if (now - led.lastChange >= 125) {
        led.phase ^= 1;
        led.lastChange = now;
      }
      applyColor(led.phase ? color : 0);
      break;

    case ANIM_PULSE: {
      // Sine-approximated breathing, period ~2s
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
          applyColor(0xFFFFFF);         // brief white flash
          if (now - led.lastChange >= 80) { led.phase = 1; led.lastChange = now; }
        } else if (led.phase == 1) {
          applyColor(0);
          if (now - led.lastChange >= 60) { led.phase = 2; led.lastChange = now; }
        } else {
          led.flashDone = true;         // fall through to solid
        }
      } else {
        applyColor(color);
      }
      break;

    case ANIM_BAR:
#ifdef LED_NEOPIXEL
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t c = (i < led.barCount) ? COLORS[led.colorIdx] : COLORS[led.barBg];
        strip.setPixelColor(i, c);
      }
      strip.show();
#else
      applyColor((led.barCount > 0) ? color : COLORS[led.barBg]);
#endif
      break;

    case ANIM_SPLIT:
#ifdef LED_NEOPIXEL
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t c = (i < led.barCount) ? COLORS[led.colorIdx] : COLORS[led.barBg];
        strip.setPixelColor(i, c);
      }
      strip.show();
#else
      applyColor((led.barCount >= 4) ? color : COLORS[led.barBg]);
#endif
      break;
  }
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

WiFiUDP  udp;
IPAddress masterIP;
uint8_t  myId         = 0;
uint32_t lastRegister = 0;
uint32_t lastPing     = 0;

void sendPkt(uint8_t type, uint8_t nid,
             uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,
             uint8_t d3=0,uint8_t d4=0,uint8_t d5=0) {
  Packet p;
  p.type = type; p.nodeId = nid;
  p.data[0]=d0; p.data[1]=d1; p.data[2]=d2;
  p.data[3]=d3; p.data[4]=d4; p.data[5]=d5;
  udp.beginPacket(masterIP, UDP_PORT);
  udp.write((uint8_t*)&p, sizeof(p));
  udp.endPacket();
}

void handleUDP() {
  int size = udp.parsePacket();
  if (size < (int)sizeof(Packet)) return;

  Packet p;
  udp.read((uint8_t*)&p, sizeof(p));

  switch (p.type) {
    case PKT_ACK:
      myId = p.data[0];
      Serial.printf("[NODE] Registered as node %u\n", myId);
      setLed(COL_GREEN, ANIM_FLASH);
      break;

    case PKT_SET_LED:
      if (p.nodeId == myId || p.nodeId == 0xFF)
        setLed(p.data[0], p.data[1]);
      break;

    case PKT_SET_BAR:
      if (p.nodeId == myId || p.nodeId == 0xFF) {
        led.colorIdx = p.data[0];
        led.anim     = ANIM_BAR;
        led.barCount = (p.data[1] < NEO_COUNT) ? p.data[1] : NEO_COUNT;
        led.barBg    = p.data[2];
      }
      break;

    case PKT_SET_SPLIT:
      if (p.nodeId == myId || p.nodeId == 0xFF) {
        led.colorIdx = p.data[0];
        led.anim     = ANIM_SPLIT;
        led.barCount = (p.data[1] < NEO_COUNT) ? p.data[1] : NEO_COUNT;
        led.barBg    = p.data[2];
      }
      break;

    case PKT_GAME_START:
      Serial.printf("[NODE] Game started, mode=%u\n", p.data[0]);
      break;

    case PKT_GAME_OVER:
      Serial.println("[NODE] Game over");
      break;

    case PKT_RESET:
      Serial.println("[NODE] Reset empfangen – re-registriere...");
      myId = 0;
      lastRegister = 0;
      setLed(COL_BLUE, ANIM_BLINK_SLOW);
      break;
  }
}

// ─────────────────────────────────────────────────────────────
// WiFi connection
// ─────────────────────────────────────────────────────────────

void connectWiFi() {
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
  if (strlen(WIFI_PASS) > 0) WiFi.begin(WIFI_SSID, WIFI_PASS);
  else WiFi.begin(WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println("\n[WARN] WiFi timeout, retrying...");
      WiFi.disconnect();
      delay(1000);
      if (strlen(WIFI_PASS) > 0) WiFi.begin(WIFI_SSID, WIFI_PASS);
      else WiFi.begin(WIFI_SSID);
      start = millis();
    }
    // Gelb blinken = sucht WLAN (noch nicht verbunden)
    applyColor((millis() / 300) % 2 ? 0x886600 : 0);
    delay(100);
    Serial.print('.');
  }

  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
  masterIP.fromString(MASTER_IP);
  udp.begin(UDP_PORT);
}

// ─────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP CTF Game – Node ===");

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

  connectWiFi();

  // ArduinoOTA – Hostname aus letzten 3 MAC-Bytes / ChipID
  {
    char hostname[20];
#ifdef ESP32
    uint64_t mac = ESP.getEfuseMac();
    snprintf(hostname, sizeof(hostname), "ctf-node-%02x%02x%02x",
             (uint8_t)(mac>>16), (uint8_t)(mac>>8), (uint8_t)mac);
#else
    snprintf(hostname, sizeof(hostname), "ctf-node-%06x", ESP.getChipId() & 0xFFFFFF);
#endif
    ArduinoOTA.setHostname(hostname);
    Serial.printf("[OTA] Hostname: %s\n", hostname);
  }
  ArduinoOTA.onStart([]() {
    setLed(COL_PURPLE, ANIM_BLINK_FAST);
    Serial.println("[OTA] Start");
  });
  ArduinoOTA.onEnd([]() {
    setLed(COL_GREEN, ANIM_SOLID);
    Serial.println("[OTA] Fertig – Neustart");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
#ifdef LED_NEOPIXEL
    uint8_t lit = (uint8_t)((uint32_t)progress * NEO_COUNT / total);
    for (int i = 0; i < NEO_COUNT; i++)
      strip.setPixelColor(i, i < lit ? 0x800080 : 0x100010);
    strip.show();
#endif
  });
  ArduinoOTA.onError([](ota_error_t e) {
    setLed(COL_RED, ANIM_BLINK_FAST);
    Serial.printf("[OTA] Fehler %u\n", e);
  });
  ArduinoOTA.begin();

  // Zufaelliges Jitter 0-2s damit Nodes nicht gleichzeitig registrieren
#ifdef ESP32
  randomSeed((uint32_t)(ESP.getEfuseMac() >> 8) ^ millis());
#else
  randomSeed(ESP.getChipId() ^ millis());
#endif
  delay(random(0, 2000));

  // Register with master
  setLed(COL_BLUE, ANIM_BLINK_SLOW);
}

void loop() {
  // WLAN-Status pruefen – kurze Aussetzer (Blips) tolerieren, NICHT sofort
  // die Registrierung wegwerfen. Erst nach 3s echtem Verlust neu verbinden.
  static uint32_t wifiLostSince = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostSince == 0) wifiLostSince = millis();
    if (millis() - wifiLostSince < 3000) {
      // Innerhalb Toleranz: WLAN reconnectet automatisch im Hintergrund.
      // myId behalten (Master kennt uns noch fuer 15s).
      updateLed();
      yield();
      return;
    }
    // Echter Verlust > 3s
    if (myId != 0) {
      Serial.println("[WARN] WiFi laenger weg – ID zurueckgesetzt");
      myId = 0; lastRegister = 0; lastPing = 0;
    }
    setLed(COL_YELLOW, ANIM_BLINK_FAST);
    connectWiFi();
    wifiLostSince = 0;
    return;
  }
  wifiLostSince = 0;  // WLAN ok

  ArduinoOTA.handle();
  handleUDP();

  // Registration (until acknowledged) – aggressiver wenn unregistriert
  uint32_t regRetry = (myId == 0) ? 1000UL : REGISTER_RETRY_MS;
  if (myId == 0 && millis() - lastRegister >= regRetry) {
    lastRegister = millis();
    sendPkt(PKT_REGISTER, 0);
    delayMicroseconds(500);
    sendPkt(PKT_REGISTER, 0);  // doppelt gegen Paketverlust
    Serial.println("[NODE] Registering...");
  }

  // Keepalive ping – doppelt senden fuer mehr Zuverlaessigkeit
  if (myId != 0 && millis() - lastPing >= PING_INTERVAL_MS) {
    lastPing = millis();
    sendPkt(PKT_PING, myId);
    delayMicroseconds(500);
    sendPkt(PKT_PING, myId);  // zweites Paket als Backup
  }

  // Button – Druck IMMER melden (Diagnose), aber nur bei Registrierung senden
  if (buttonPressed()) {
    if (myId != 0) {
      Serial.printf("[NODE] Button pressed (id=%u) -> gesendet\n", myId);
      sendPkt(PKT_BUTTON, myId);
    } else {
      Serial.println("[NODE] Button erkannt, aber noch nicht registriert (myId=0)");
    }
  }

  updateLed();
  yield();  // ESP8266 watchdog
}
