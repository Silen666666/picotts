#pragma once

// ================================================================
// LED type – uncomment exactly ONE
// ================================================================
#define LED_NEOPIXEL          // WS2812B / WS2811 addressable LED
// #define LED_RGB_CATHODE    // Common-cathode RGB LED (3 PWM pins)
// #define LED_RGB_ANODE      // Common-anode RGB LED  (3 PWM pins, inverted)

// ----------------------------------------------------------------
// Pin assignments
// ----------------------------------------------------------------
#ifdef ESP32
  // NeoPixel
  #define NEO_PIN    13
  // Simple RGB LED
  #define LED_R_PIN  25
  #define LED_G_PIN  26
  #define LED_B_PIN  27
  // Button (active LOW, internal pull-up)
  #define BUTTON_PIN  0
#else  // ESP8266 NodeMCU / Wemos D1 mini
  // NeoPixel
  #define NEO_PIN     2    // D4 (GPIO2)
  // Simple RGB LED
  #define LED_R_PIN  14    // D5
  #define LED_G_PIN  12    // D6
  #define LED_B_PIN  13    // D7
  // Button (active LOW, internal pull-up)
  #define BUTTON_PIN  0    // D3 (GPIO0) – avoid using GPIO0 at boot if possible
#endif

#define NEO_COUNT          8   // WS2812B-8 bar
#define BUTTON_DEBOUNCE_MS 50

// ----------------------------------------------------------------
// Network
// ----------------------------------------------------------------
#define WIFI_SSID          "ESP-CTF-Game"
#define WIFI_PASS          ""
#define MASTER_IP          "192.168.4.1"

#define REGISTER_RETRY_MS  2000
#define PING_INTERVAL_MS   3000
#define WIFI_TIMEOUT_MS   15000
