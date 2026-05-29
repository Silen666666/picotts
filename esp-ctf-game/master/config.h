#pragma once

// WiFi AP created by the master
#define WIFI_SSID        "ESP-CTF-Game"
#define WIFI_PASS        ""              // open network

#define MAX_NODES        16
#define NODE_TIMEOUT_MS  8000UL

// CTF
#define CTF_TEAMS        2              // 2, 3, or 4
#define CTF_DURATION_S   180            // seconds

// Bomb Defusal
#define BOMB_SEQ_LEN     5              // steps to disarm
#define BOMB_DURATION_S  120
#define BOMB_STEP_MS     1200           // ms per step in sequence reveal
