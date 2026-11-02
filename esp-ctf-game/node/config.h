#pragma once

// ================================================================
// LED type – uncomment exactly ONE
// ================================================================
#define LED_NEOPIXEL          // WS2812B / WS2811 addressable LED
// #define LED_RGB_CATHODE    // Common-cathode RGB LED (3 PWM pins)
// #define LED_RGB_ANODE      // Common-anode RGB LED  (3 PWM pins, inverted)

// ----------------------------------------------------------------
// Pin assignments (ESP32 NodeMCU, 30 Pins)
// ----------------------------------------------------------------
// NeoPixel – D13 header pin on 30-pin NodeMCU
#define NEO_PIN    13   // labeled D13 on board
// Simple RGB LED
#define LED_R_PIN  25
#define LED_G_PIN  26
#define LED_B_PIN  27
// Button (active LOW, internal pull-up) – D18 header pin (GPIO0/BOOT not on header)
#define BUTTON_PIN  18  // labeled D18 on board

#define NEO_COUNT          8   // WS2812B-8 bar
#define BUTTON_DEBOUNCE_MS 50

// ----------------------------------------------------------------
// Master-WLAN – MUSS zum Master passen (master/config.h)!
// Gespielt wird per ESP-NOW; das WLAN dient nur dazu, den Funkkanal
// des Masters zu finden, und fuer Firmware-Updates vom Master.
// ----------------------------------------------------------------
#define WIFI_SSID          "ESP-CTF-Game"
#define WIFI_PASS          ""
#define MASTER_IP          "192.168.4.1"   // Download-Adresse fuer Updates (nicht aendern)

// (Registrierung, Status alle 2 s und Suchzeiten sind fest im Sketch –
//  sie gehoeren zum Protokoll mit dem Master.)

// ----------------------------------------------------------------
// Stabilitaet
// ----------------------------------------------------------------
#define USE_WATCHDOG   1      // 1 = Node startet neu, falls die Firmware haengt
#define WDT_TIMEOUT_S  30     // Watchdog-Zeit in Sekunden
